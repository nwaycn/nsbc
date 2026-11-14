//! Sound file management for dialplan playback/ringback (REQ-DP-UPLOAD).
//!
//! Four endpoints (no new Rust deps beyond axum/tokio — server builds offline):
//!   GET    /sounds           — list files in sounds_root (.wav/.mp3/.opus/.gsm)
//!   PUT    /sounds/:name     — upload (raw request body = file bytes; 16 MB max)
//!   DELETE /sounds/:name     — delete a sound file
//!   GET    /sounds/:name     — download / audio preview

use std::path::{Path, PathBuf};
use std::sync::Arc;

use axum::{
    body::Bytes,
    extract::{Path as AxumPath, State},
    http::StatusCode,
    response::{IntoResponse, Response},
    routing::{get, put},
    Json, Router,
};
use serde::{Deserialize, Serialize};
use serde_json::json;

use crate::coreclient::CoreApi;
use crate::error::ApiError;
use crate::AppState;

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: CoreApi + Clone + Send + Sync + 'static,
{
    Router::new()
        .route("/sounds", get(list::<C>))
        .route("/sounds/:name", get(download::<C>).put(upload::<C>).delete(delete_one::<C>))
}

const ALLOWED_EXT: &[&str] = &["wav", "mp3", "opus", "gsm"];
const MAX_UPLOAD_BYTES: usize = 16 * 1024 * 1024; /* 16 MB */

#[derive(Debug, Serialize, Deserialize)]
pub struct SoundFile {
    pub name:  String,
    pub size:  u64,
    pub mtime: u64,
}

fn is_allowed_ext(name: &str) -> bool {
    let lower = name.to_ascii_lowercase();
    ALLOWED_EXT.iter().any(|e| lower.ends_with(&format!(".{e}")))
}

fn safe_join(root: &Path, name: &str) -> Result<PathBuf, String> {
    if name.is_empty() {
        return Err("empty filename".into());
    }
    if name.contains('/') || name.contains('\\') {
        return Err(format!("path separator in filename '{name}'"));
    }
    if name.contains("..") {
        return Err(format!("'..' in filename '{name}'"));
    }
    let candidate = root.join(name);
    let root_canon = root.canonicalize()
        .map_err(|e| format!("canonicalize root {}: {e}", root.display()))?;

    if candidate.exists() {
        let canon = candidate.canonicalize()
            .map_err(|e| format!("canonicalize {}: {e}", candidate.display()))?;
        if !canon.starts_with(&root_canon) {
            return Err(format!("path escapes root: {}", candidate.display()));
        }
        Ok(canon)
    } else {
        // Upload path — file does not exist yet. Verify parent is inside root.
        let parent = candidate.parent()
            .ok_or_else(|| format!("no parent for {}", candidate.display()))?;
        let parent_canon = parent.canonicalize()
            .map_err(|e| format!("canonicalize parent {}: {e}", parent.display()))?;
        if !parent_canon.starts_with(&root_canon) {
            return Err(format!("parent escapes root: {}", parent.display()));
        }
        Ok(candidate)
    }
}

/// GET /sounds — list files
async fn list<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
) -> Result<impl IntoResponse, ApiError> {
    let root = &st.sounds_root;
    let mut files: Vec<SoundFile> = Vec::new();

    if !root.exists() {
        return Ok(Json(json!({ "files": [], "root": root.display().to_string() })));
    }

    let mut rd = std::fs::read_dir(root).map_err(|e| {
        ApiError::Db(format!("read_dir sounds_root: {e}"))
    })?;

    while let Some(entry) = rd.next().transpose().map_err(|e| {
        ApiError::Db(format!("read_dir entry: {e}"))
    })? {
        let path = entry.path();
        if !path.is_file() { continue; }
        let name = path.file_name().and_then(|s| s.to_str()).unwrap_or("").to_string();
        if !is_allowed_ext(&name) { continue; }

        let Ok(meta) = entry.metadata() else { continue };
        let mtime: u64 = meta
            .modified()
            .ok()
            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
            .map(|d| d.as_secs())
            .unwrap_or(0);
        files.push(SoundFile { name, size: meta.len(), mtime });
    }

    files.sort_by(|a, b| a.name.cmp(&b.name));

    Ok(Json(json!({
        "files": files,
        "root": root.display().to_string(),
        "allowed_ext": ALLOWED_EXT,
    })))
}

/// Generate a collision-free name: stem + "_" + 6-hex suffix + extension.
/// Suffix is derived from SystemTime nanos (lowest 24 bits). Retries up to 50
/// times if there's still a collision (practically impossible under normal
/// load). No new deps needed.
fn unique_name_if_exists(root: &Path, name: &str) -> String {
    let candidate = root.join(name);
    if !candidate.exists() {
        return name.to_string();
    }
    let orig = Path::new(name);
    let stem  = orig.file_stem().and_then(|s| s.to_str()).unwrap_or("sound");
    let ext   = orig.extension().and_then(|s| s.to_str()).unwrap_or("wav");
    for _ in 0..50 {
        let t = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos() as u64)
            .unwrap_or(0);
        let suffix = format!("{:06x}", t & 0xFFFFFF);
        let new_name = format!("{stem}_{suffix}.{ext}");
        if !root.join(&new_name).exists() {
            return new_name;
        }
        std::thread::yield_now();
    }
    // Ultimate fallback — append random counter
    format!("{stem}_{}_{ext}", "ZZZZZZ")
}

/// PUT /sounds/:name — upload (body = raw file bytes)
///
/// No multipart dep: caller sends the file body directly.
/// If a file with the same name already exists, the server automatically
/// generates a collision-free unique name (`stem_<6hex>.ext`) and returns
/// it to the caller. This lets multiple dialplan rules have their own copy
/// of "greeting.wav" without stomping each other.
async fn upload<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    AxumPath(name): AxumPath<String>,
    body: Bytes,
) -> Result<impl IntoResponse, ApiError> {
    tracing::info!(name = %name, size = body.len(), "sounds/upload hit");

    if !is_allowed_ext(&name) {
        tracing::warn!(name = %name, "sounds/upload rejected: bad extension");
        return Err(ApiError::BadRequest(format!(
            "invalid file extension '.{}' — allowed: {}",
            Path::new(&name).extension().and_then(|e| e.to_str()).unwrap_or("?"),
            ALLOWED_EXT.join(", ")
        )));
    }
    if body.len() > MAX_UPLOAD_BYTES {
        return Err(ApiError::BadRequest(format!(
            "file too large (limit {} MB, got {} MB)",
            MAX_UPLOAD_BYTES / (1024 * 1024),
            body.len() / (1024 * 1024)
        )));
    }

    let root = st.sounds_root.clone();
    tracing::info!(root = %root.display(), "sounds root");
    if !root.exists() {
        tracing::info!(path = %root.display(), "creating sounds_root");
        std::fs::create_dir_all(&root).map_err(|e| {
            ApiError::Db(format!("create sounds_root: {e}"))
        })?;
    }

    // Auto-rename on collision — multiple rules uploading "greeting.wav"
    // each gets its own unique copy instead of overwriting the previous one.
    let final_name = unique_name_if_exists(&root, &name);
    if final_name != name {
        tracing::info!(orig = %name, assigned = %final_name, "sounds/upload renamed to avoid collision");
    }

    let dest = safe_join(&root, &final_name).map_err(|reason| {
        tracing::warn!(name = %final_name, reason = %reason, "safe_join failed");
        ApiError::BadRequest(reason)
    })?;

    tracing::info!(path = %dest.display(), "writing upload");
    tokio::fs::write(&dest, &body).await.map_err(|e| {
        ApiError::Db(format!("write {}: {e}", dest.display()))
    })?;

    let size = body.len();
    tracing::info!(path = %dest.display(), size, "sound uploaded OK");

    Ok(Json(json!({
        "name": final_name,
        "path": dest.display().to_string(),
        "size": size,
    })))
}

/// DELETE /sounds/:name
async fn delete_one<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    AxumPath(name): AxumPath<String>,
) -> Result<impl IntoResponse, ApiError> {
    let root = &st.sounds_root;
    let target = safe_join(root, &name).map_err(|r| ApiError::BadRequest(r))?;
    if !target.exists() {
        return Err(ApiError::NotFound(format!("sound file not found: {name}")));
    }
    std::fs::remove_file(&target).map_err(|e| {
        ApiError::Db(format!("remove {}: {e}", target.display()))
    })?;
    tracing::info!(path = %target.display(), "sound deleted");
    Ok(StatusCode::NO_CONTENT)
}

/// GET /sounds/:name — download / audio preview
async fn download<C: CoreApi + Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    AxumPath(name): AxumPath<String>,
) -> Result<Response, ApiError> {
    let root = &st.sounds_root;
    let target = safe_join(root, &name).map_err(|r| ApiError::BadRequest(r))?;
    if !target.exists() {
        return Err(ApiError::NotFound(format!("sound file not found: {name}")));
    }

    let mime = match target.extension().and_then(|e| e.to_str()) {
        Some("wav")  => "audio/wav",
        Some("mp3")  => "audio/mpeg",
        Some("opus") => "audio/ogg",
        Some("gsm")  => "audio/gsm",
        _ => "application/octet-stream",
    };

    let bytes = std::fs::read(&target).map_err(|e| {
        ApiError::Db(format!("read {}: {e}", target.display()))
    })?;

    Ok((
        StatusCode::OK,
        [("content-type", mime),
         ("content-disposition",
          &format!("inline; filename=\"{name}\""))],
        bytes,
    ).into_response())
}
