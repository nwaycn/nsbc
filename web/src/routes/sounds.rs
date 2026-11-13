//! Sound file management for dialplan playback/ringback (REQ-DP-UPLOAD).
//!
//! Four endpoints:
//!   GET    /sounds           — list .wav files in sounds_root
//!   POST   /sounds/upload    — multipart upload (.wav/.mp3/.opus/.gsm, 16 MB max)
//!   DELETE /sounds/:name     — delete a sound file
//!   GET    /sounds/:name     — download (in-browser audio preview)

use std::path::{Path, PathBuf};
use std::sync::Arc;

use axum::{
    extract::{Multipart, Path as AxumPath, State},
    http::StatusCode,
    response::{IntoResponse, Response},
    routing::{get, post},
    Json, Router,
};
use serde::{Deserialize, Serialize};
use serde_json::json;

use crate::error::ApiError;
use crate::AppState;

pub fn router<C>() -> Router<Arc<AppState<C>>>
where
    C: Clone + Send + Sync + 'static,
{
    Router::new()
        .route("/sounds", get(list::<C>))
        .route("/sounds/upload", post(upload::<C>))
        .route("/sounds/:name", get(download::<C>).delete(delete_one::<C>))
}

/// Whitelist — only these extensions allowed to upload.
const ALLOWED_EXT: &[&str] = &["wav", "mp3", "opus", "gsm"];
const MAX_UPLOAD_BYTES: usize = 16 * 1024 * 1024; /* 16 MB — way bigger than any prompt */

#[derive(Debug, Serialize, Deserialize)]
pub struct SoundFile {
    pub name:  String,
    pub size:  u64,
    /// Modification time as UNIX epoch seconds (0 if unavailable).
    pub mtime: u64,
}

fn is_allowed_ext(name: &str) -> bool {
    let lower = name.to_ascii_lowercase();
    ALLOWED_EXT.iter().any(|e| lower.ends_with(&format!(".{e}")))
}

fn safe_join(root: &Path, name: &str) -> Option<PathBuf> {
    // 1) No path separators allowed — block ../../ etc.
    if name.contains('/') || name.contains('\\') || name.contains("..") {
        return None;
    }
    // 2) Build + canonicalize
    let candidate = root.join(name);
    let canon = candidate.canonicalize().ok()?;
    // 3) Must still be inside root after canonicalize
    let root_canon = root.canonicalize().ok()?;
    if !canon.starts_with(&root_canon) { return None; }
    Some(canon)
}

/// GET /sounds — list files
async fn list<C: Clone + Send + Sync + 'static>(
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

        let meta = match entry.metadata() {
            Ok(m) => m,
            Err(_) => continue,
        };
        let mtime: u64 = meta
            .modified()
            .ok()
            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
            .map(|d| d.as_secs())
            .unwrap_or(0);
        files.push(SoundFile { name, size: meta.len(), mtime });
    }

    // Sort by name ascending
    files.sort_by(|a, b| a.name.cmp(&b.name));

    Ok(Json(json!({
        "files": files,
        "root": root.display().to_string(),
        "allowed_ext": ALLOWED_EXT,
    })))
}

/// POST /sounds/upload — multipart
async fn upload<C: Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    mut multipart: Multipart,
) -> Result<impl IntoResponse, ApiError> {
    let root = st.sounds_root.clone();
    if !root.exists() {
        std::fs::create_dir_all(&root).map_err(|e| {
            ApiError::Db(format!("create sounds_root: {e}"))
        })?;
    }

    let mut saved_name: Option<String> = None;
    let mut total_bytes: usize = 0;

    while let Some(field) = multipart.next_field().await.map_err(|e| {
        ApiError::BadRequest(format!("multipart error: {e}"))
    })? {
        let orig_name = field.file_name().unwrap_or("upload.wav").to_string();
        let is_sound = is_allowed_ext(&orig_name);
        let field_name = field.name().unwrap_or("file").to_string();

        if field_name != "file" || !is_sound {
            if field_name == "file" && !is_sound {
                return Err(ApiError::BadRequest(format!(
                    "invalid file extension '.{}' — allowed: {}",
                    Path::new(&orig_name).extension()
                        .and_then(|e| e.to_str()).unwrap_or("?"),
                    ALLOWED_EXT.join(", ")
                )));
            }
            continue;
        }

        let safe_name = Path::new(&orig_name)
            .file_name()
            .and_then(|s| s.to_str())
            .unwrap_or("upload.wav")
            .to_string();

        let dest = safe_join(&root, &safe_name).ok_or_else(|| {
            ApiError::BadRequest(format!("invalid filename: {safe_name}"))
        })?;

        use tokio::io::AsyncWriteExt;
        let mut file = tokio::fs::File::create(&dest).await.map_err(|e| {
            ApiError::Db(format!("create {}: {e}", dest.display()))
        })?;

        while let Some(chunk) = field.chunk().await.map_err(|e| {
            ApiError::BadRequest(format!("read chunk: {e}"))
        })? {
            total_bytes += chunk.len();
            if total_bytes > MAX_UPLOAD_BYTES {
                let _ = tokio::fs::remove_file(&dest).await;
                return Err(ApiError::BadRequest(format!(
                    "file too large (limit {} MB)",
                    MAX_UPLOAD_BYTES / (1024 * 1024)
                )));
            }
            file.write_all(&chunk).await.map_err(|e| {
                ApiError::Db(format!("write chunk: {e}"))
            })?;
        }
        file.flush().await.ok();
        drop(file);

        tracing::info!(path = %dest.display(), size = total_bytes,
            "sound uploaded");

        saved_name = Some(safe_name);
    }

    let Some(name) = saved_name else {
        return Err(ApiError::BadRequest(
            "no file field in multipart (expected field name 'file')".into(),
        ));
    };

    let abs_path = root.join(&name);
    Ok(Json(json!({
        "name": name,
        "path": abs_path.display().to_string(),
        "size": total_bytes,
    })))
}

/// DELETE /sounds/:name
async fn delete_one<C: Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    AxumPath(name): AxumPath<String>,
) -> Result<impl IntoResponse, ApiError> {
    let root = &st.sounds_root;
    let target = safe_join(root, &name).ok_or_else(|| {
        ApiError::BadRequest(format!("invalid filename: {name}"))
    })?;
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
async fn download<C: Clone + Send + Sync + 'static>(
    State(st): State<Arc<AppState<C>>>,
    AxumPath(name): AxumPath<String>,
) -> Result<Response, ApiError> {
    let root = &st.sounds_root;
    let target = safe_join(root, &name).ok_or_else(|| {
        ApiError::BadRequest(format!("invalid filename: {name}"))
    })?;
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
