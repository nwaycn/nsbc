//! TLS 证书管理：自签 / ACME + 热切换 + 后台自动续期。
//!
//! 三种证书来源（互斥，按优先级）：
//!   1. **公网 ACME**    — tls.public=true + 启用 acme feature → certon TLS-ALPN-01
//!                         certon 自管 ServerConfig（动态 ResolvesServerCert）
//!                         后台 pem_bridge 同步 PEM 到磁盘 → kill nsbcd SIGHUP
//!   2. **显式 PEM**     — cert_path + key_path 文件都存在 → 直接用（不续期）
//!   3. **内网自签**     — auto_generate=true → rcgen 生成 + 后台每小时查过期自动续
//!
//! 热切换机制：
//!   - **内部模式**：RwLock<Arc<TlsAcceptor>> 原子替换
//!   - **公网模式**：certon ServerConfig 内置 ResolvesServerCert，每次 TLS 握手自动从 cache 取最新

use std::net::SocketAddr;
use std::path::{Path, PathBuf};
use std::sync::{Arc, RwLock};

use rcgen::{BasicConstraints, CertificateParams, DnType, Ia5String, IsCa, KeyPair, SanType};
use rustls::{ServerConfig, pki_types::{CertificateDer, PrivateKeyDer}};
use tokio_rustls::TlsAcceptor;

use crate::config::TlsSection;

/// (cert_pem_bytes, key_pem_bytes)
pub type CertPair = (Vec<u8>, Vec<u8>);

/// 热切换 acceptor 容器
pub type AcceptorHolder = Arc<RwLock<Arc<TlsAcceptor>>>;

/// TLS 来源枚举 — main.rs 根据此构建 accept loop
pub enum TlsSource {
    /// 内部自签或显式 PEM —— 用 RwLock 热换
    Internal {
        holder: AcceptorHolder,
        renew_task: Option<tokio::task::JoinHandle<()>>,
    },
    /// 公网 ACME —— certon TLS-ALPN-01 自管 ServerConfig（动态 ResolvesServerCert）
    #[cfg(feature = "acme")]
    Public {
        rustls_config: Arc<ServerConfig>,
    },
    /// 禁用 TLS —— 纯 HTTP
    PlainHttp,
}

// =====================================================================
// 入口：启动时加载
// =====================================================================

pub async fn init(
    tls: &TlsSection,
    bind: SocketAddr,
) -> Result<TlsSource, String> {

    if !tls.enabled {
        return Ok(TlsSource::PlainHttp);
    }

    // ---- 公网 ACME 模式 ----
    #[cfg(feature = "acme")]
    if tls.public {
        return init_public_acme(tls, bind).await;
    }

    #[cfg(not(feature = "acme"))]
    if tls.public {
        tracing::warn!(
            "tls.public=true 但当前构建未启用 acme feature → fallback 内网自签；\
             如需真实 ACME：cargo build --features acme"
        );
        // Fallback: 强制 auto_generate=true，确保 cert/key 不存在时能自签
        let mut fallback = tls.clone();
        fallback.auto_generate = true;
        return init_internal(&fallback, bind).await;
    }

    // ---- 默认：内部自签或显式 PEM ----
    init_internal(tls, bind).await
}

// =====================================================================
// 内部模式（自签 / 显式 PEM）
// =====================================================================

async fn init_internal(
    tls: &TlsSection,
    bind: SocketAddr,
) -> Result<TlsSource, String> {
    let cert_pair = load_or_generate_certs(tls, bind)?;
    let acceptor = build_acceptor(&cert_pair.0, &cert_pair.1)?;
    let holder: AcceptorHolder = Arc::new(RwLock::new(Arc::new(acceptor)));

    // 显式 PEM 用户自己续 —— 我们只管 auto_generate
    let renew_task = if tls.auto_generate {
        Some(tokio::spawn(cert_renew_loop(
            tls.clone(),
            bind,
            holder.clone(),
        )))
    } else {
        None
    };

    Ok(TlsSource::Internal { holder, renew_task })
}

pub fn load_or_generate_certs(tls: &TlsSection, bind: SocketAddr) -> Result<CertPair, String> {
    let cert_exists = tls.cert_path.exists();
    let key_exists  = tls.key_path.exists();

    if cert_exists && key_exists {
        tracing::info!(
            cert = %tls.cert_path.display(),
            key  = %tls.key_path.display(),
            "using existing TLS cert/key"
        );
        return load_pems(&tls.cert_path, &tls.key_path);
    }

    if tls.auto_generate {
        tracing::warn!(
            cert = %tls.cert_path.display(),
            key  = %tls.key_path.display(),
            "cert/key missing — generating self-signed TLS cert"
        );
        let (cert, key) = generate_self_signed(tls, bind)?;
        let not_after_unix = time::OffsetDateTime::now_utc().unix_timestamp()
            + tls.validity_days.max(1) as i64 * 86400;
        write_pems(&tls.cert_path, &tls.key_path, &cert, &key, not_after_unix)?;
        Ok((cert, key))
    } else {
        Err(format!(
            "TLS enabled but cert/key not found at {} / {} (auto_generate=false)",
            tls.cert_path.display(), tls.key_path.display()
        ))
    }
}

fn load_pems(cert_path: &Path, key_path: &Path) -> Result<CertPair, String> {
    let cert = std::fs::read(cert_path)
        .map_err(|e| format!("read cert {}: {e}", cert_path.display()))?;
    let key = std::fs::read(key_path)
        .map_err(|e| format!("read key {}: {e}", key_path.display()))?;
    Ok((cert, key))
}

fn write_pems(
    cert_path: &Path, key_path: &Path, cert: &[u8], key: &[u8],
    not_after_unix: i64,
) -> Result<(), String> {
    if let Some(dir) = cert_path.parent() {
        if !dir.as_os_str().is_empty() {
            std::fs::create_dir_all(dir)
                .map_err(|e| format!("create cert dir {}: {e}", dir.display()))?;
        }
    }

    std::fs::write(cert_path, cert)
        .map_err(|e| format!("write cert {}: {e}", cert_path.display()))?;
    std::fs::write(key_path, key)
        .map_err(|e| format!("write key {}: {e}", key_path.display()))?;

    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = std::fs::set_permissions(key_path, std::fs::Permissions::from_mode(0o600));
    }

    let meta_path = cert_path.with_extension("meta.json");
    let meta = format!(
        "{{\"not_after_unix\":{not_after_unix},\"generated_at_unix\":{}}}",
        time::OffsetDateTime::now_utc().unix_timestamp()
    );
    std::fs::write(&meta_path, meta.as_bytes())
        .map_err(|e| format!("write cert meta {}: {e}", meta_path.display()))?;

    Ok(())
}

fn build_acceptor(cert_pem: &[u8], key_pem: &[u8]) -> Result<TlsAcceptor, String> {
    let cert_chain: Vec<CertificateDer<'static>> = rustls_pemfile::certs(&mut &cert_pem[..])
        .collect::<Result<Vec<_>, _>>()
        .map_err(|e| format!("parse cert pem: {e}"))?;
    let Some(key): Option<PrivateKeyDer<'static>> = rustls_pemfile::private_key(&mut &key_pem[..])
        .map_err(|e| format!("parse key pem: {e}"))?
    else {
        return Err("no private key in key pem".into());
    };
    let rustls_config = ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(cert_chain, key)
        .map_err(|e| format!("rustls config: {e}"))?;
    Ok(TlsAcceptor::from(Arc::new(rustls_config)))
}

// =====================================================================
// 自签证书生成（rcgen）
// =====================================================================

fn generate_self_signed(tls: &TlsSection, bind: SocketAddr) -> Result<CertPair, String> {
    let mut params = CertificateParams::new(vec![])
        .map_err(|e| format!("cert params: {e}"))?;

    params.distinguished_name.push(DnType::CommonName, "nsbc-web");
    params.distinguished_name.push(DnType::OrganizationName,  "NSBC");
    params.distinguished_name.push(DnType::OrganizationalUnitName, "self-signed");

    let days = tls.validity_days.max(1) as i64;
    params.not_after = time::OffsetDateTime::now_utc() + time::Duration::days(days);
    params.not_before = time::OffsetDateTime::now_utc() - time::Duration::hours(1);
    params.is_ca = IsCa::Ca(BasicConstraints::Unconstrained);

    let san_strs = collect_sans(tls, bind);
    for s in &san_strs {
        params.subject_alt_names.push(
            if s.parse::<std::net::IpAddr>().is_ok() {
                SanType::IpAddress(s.parse().unwrap())
            } else {
                SanType::DnsName(Ia5String::try_from(s.as_str())
                    .map_err(|e| format!("invalid SAN '{s}': {e}"))?)
            }
        );
    }

    let key = KeyPair::generate()
        .map_err(|e| format!("keypair generate: {e}"))?;
    let cert = params.self_signed(&key)
        .map_err(|e| format!("self-sign: {e}"))?;

    let cert_pem = cert.pem();
    let key_pem  = key.serialize_pem();

    tracing::info!(
        sans = ?san_strs,
        validity_days = days,
        "self-signed TLS cert generated"
    );

    Ok((cert_pem.into_bytes(), key_pem.into_bytes()))
}

fn collect_sans(tls: &TlsSection, bind: SocketAddr) -> Vec<String> {
    if !tls.san.trim().is_empty() {
        return tls.san.split(',').map(|s| s.trim().to_string()).collect();
    }
    let mut v = Vec::new();
    let host = bind.ip().to_string();
    v.push(host.clone());
    if bind.ip().is_unspecified() {
        v.push("127.0.0.1".into());
        v.push("localhost".into());
    }
    if tls.public && !tls.domain.is_empty() {
        v.push(tls.domain.clone());
    }
    v
}

// =====================================================================
// 后台续期循环（内部自签模式）
// =====================================================================

pub async fn cert_renew_loop(
    tls: TlsSection,
    bind: SocketAddr,
    holder: AcceptorHolder,
) {
    let interval_secs = tls.check_interval_secs.max(60);
    let threshold_hours = tls.renew_threshold_hours.max(1);

    tracing::info!(
        interval = interval_secs,
        threshold_hours,
        "cert auto-renew loop started (self-signed)"
    );

    if let Err(e) = try_renew_if_expiring(&tls, bind, &holder, threshold_hours).await {
        tracing::error!(error = %e, "initial expiry check failed");
    }

    let mut ticker = tokio::time::interval(
        tokio::time::Duration::from_secs(interval_secs)
    );
    ticker.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);

    loop {
        ticker.tick().await;
        if let Err(e) = try_renew_if_expiring(&tls, bind, &holder, threshold_hours).await {
            tracing::error!(error = %e, "cert renew cycle failed (will retry next interval)");
        }
    }
}

async fn try_renew_if_expiring(
    tls: &TlsSection,
    bind: SocketAddr,
    holder: &AcceptorHolder,
    threshold_hours: u64,
) -> Result<(), String> {
    match read_cert_meta(&tls.cert_path) {
        Ok(Some(hours_left)) => {
            if hours_left <= threshold_hours as f64 {
                tracing::warn!(
                    hours_left = format!("{:.1}", hours_left),
                    threshold_hours,
                    "cert expiring in {hours_left:.1}h ≤ threshold {threshold_hours}h — renewing"
                );
                do_renew(tls, bind, holder).await?;
            } else {
                tracing::debug!(hours_left = format!("{:.1}", hours_left), "cert OK");
            }
        }
        _ => {
            tracing::warn!("cert meta missing/unreadable — forcing regenerate");
            do_renew(tls, bind, holder).await?;
        }
    }
    Ok(())
}

async fn do_renew(
    tls: &TlsSection,
    bind: SocketAddr,
    holder: &AcceptorHolder,
) -> Result<(), String> {
    let (cert, key) = generate_self_signed(tls, bind)?;
    let not_after_unix = time::OffsetDateTime::now_utc().unix_timestamp()
        + tls.validity_days.max(1) as i64 * 86400;
    write_pems(&tls.cert_path, &tls.key_path, &cert, &key, not_after_unix)?;

    let new_acceptor = build_acceptor(&cert, &key)?;
    {
        let mut guard = holder.write().map_err(|e| format!("holder lock poisoned: {e}"))?;
        *guard = Arc::new(new_acceptor);
    }
    tracing::info!("✅ TLS cert renewed + hot-swapped (self-signed)");
    notify_nsbcd_reload();
    Ok(())
}

fn read_cert_meta(cert_path: &Path) -> Result<Option<f64>, String> {
    let meta_path = cert_path.with_extension("meta.json");
    let data = std::fs::read_to_string(&meta_path)
        .map_err(|e| format!("read meta {}: {e}", meta_path.display()))?;

    let needle = "\"not_after_unix\":";
    let Some(pos) = data.find(needle) else {
        return Err("no not_after_unix in meta".into());
    };
    let rest = &data[pos + needle.len()..];
    let end = rest.find(|c: char| !c.is_ascii_digit() && c != '-')
        .unwrap_or(rest.len());
    let not_after_unix: i64 = rest[..end].trim().parse()
        .map_err(|e| format!("parse not_after_unix: {e}"))?;

    let now = time::OffsetDateTime::now_utc().unix_timestamp();
    let diff_secs = not_after_unix - now;
    Ok(Some(diff_secs as f64 / 3600.0))
}

// =====================================================================
// 公网 ACME（certon TLS-ALPN-01）
// =====================================================================

/// certon 公网模式：TLS-ALPN-01 + 动态 ResolvesServerCert + PEM 桥接 nsbcd。
///
/// certon（Rust 版 Caddy certmagic）自动完成：
///   - 向 Let's Encrypt/ZeroSSL 申请证书（TLS-ALPN-01 挑战，只用 443/8443 端口）
///   - 后台自动续期（默认剩 1/3 有效期时续）
///   - 文件持久化（FileStorage）
///   - ServerConfig 内置 ResolvesServerCert，每次 TLS 握手自动从 cache 取最新
///
/// nsbcd 桥接：启动后台 task 轮询 certon FileStorage 目录下的证书文件（PEM 格式），
/// 发现变化 → 复制到 ../tls/cert.pem + key.pem → kill nsbcd SIGHUP
#[cfg(feature = "acme")]
async fn init_public_acme(
    tls: &TlsSection,
    bind: SocketAddr,
) -> Result<TlsSource, String> {
    if tls.domain.is_empty() {
        return Err("tls.domain must be set when public=true".into());
    }

    let cache_dir = resolve_cert_path(Path::new(
        if tls.acme_cache_dir.is_empty() { "../acme-cache" } else { &tls.acme_cache_dir }
    ));
    std::fs::create_dir_all(&cache_dir)
        .map_err(|e| format!("create acme cache dir {}: {e}", cache_dir.display()))?;

    tracing::info!(
        domain = %tls.domain,
        staging = tls.acme_staging,
        email = %tls.acme_email,
        cache = %cache_dir.display(),
        "🚀 启动 certon TLS-ALPN-01（公网 ACME）"
    );

    // === certon CertManager 配置（certon 0.3 真实 API）===
    use certon::{
        CertManager, AcmeIssuer, AcmeIssuerBuilder, FileStorage,
        LETS_ENCRYPT_PRODUCTION, LETS_ENCRYPT_STAGING,
    };
    use std::sync::Arc;

    // 1) FileStorage（certon 账户 + 证书持久化，避免每次重启重新申请）
    let storage: Arc<dyn certon::Storage> = Arc::new(FileStorage::new(cache_dir.clone()));

    // 2) AcmeIssuer（Let's Encrypt）
    let issuer_builder: AcmeIssuerBuilder = AcmeIssuer::builder();
    let issuer_builder = issuer_builder.storage(storage.clone());

    let issuer_builder = if tls.acme_staging {
        tracing::warn!("⚠️  用 Let's Encrypt staging CA（测试用，生产请关掉 acme_staging）");
        issuer_builder.test_ca(LETS_ENCRYPT_STAGING).ca(String::new())
    } else {
        issuer_builder.ca(LETS_ENCRYPT_PRODUCTION)
    };

    let issuer_builder = if !tls.acme_email.is_empty() {
        issuer_builder.email(tls.acme_email.clone()).agreed(true)
    } else {
        issuer_builder
    };

    let issuer: Arc<dyn certon::CertIssuer> = Arc::new(
        issuer_builder
            .disable_http_challenge(true)       // 只走 TLS-ALPN-01（不需 port 80）
            .disable_tlsalpn_challenge(false)
            .build()
    );

    // 3) CertManager（certon 0.3 API：issuers 收 Vec，storage 单独设）
    let manager = CertManager::builder()
        .storage(storage)
        .issuers(vec![issuer])
        .interactive(false)
        .build();   // ← build() -> CertManager，不返回 Result（API 变更）

    tracing::info!(
        domain = %tls.domain,
        staging = tls.acme_staging,
        "⏳ certon 首次申请证书（TLS-ALPN-01）— 可能需要 5-15s"
    );

    // 4) manage 域名 —— 申请（或从 storage 加载）证书 + 加入内存 cache
    manager
        .manage(&[tls.domain.clone()])
        .await
        .map_err(|e| format!(
            "certon 申请证书失败 — 请确认 {domain} 的 DNS A 记录 + 8443 端口对外可达，\
             或先开 acme_staging=true 测试。error: {e}",
            domain = tls.domain
        ))?;

    // 5) 构造带动态 resolver 的 ServerConfig（CertResolver 从 manager.cache 取）
    use certon::handshake::CertResolver;
    let resolver = CertResolver::new(manager.cache.clone());
    let rustls_config = rustls::ServerConfig::builder()
        .with_no_client_auth()
        .with_cert_resolver(Arc::new(resolver));

    tracing::info!(
        domain = %tls.domain,
        "✅ certon ACME 证书已就绪（TLS-ALPN-01）— ServerConfig 动态 resolver 工作中"
    );

    // === PEM 桥接 nsbcd ===
    // certon 把证书存到 FileStorage 目录下，格式通常是 <domain>/cert.pem + key.pem
    // 我们轮询那个目录，发现 cert bytes 变了就覆盖我们的 cert_path + kill nsbcd
    let cert_path = tls.cert_path.clone();
    let key_path  = tls.key_path.clone();
    let domain = tls.domain.clone();
    let pem_bridge_task = tokio::spawn(async move {
        // 等 certon 第一次 manage 完成后，FileStorage 里应该有证书了
        // 先 sleep 30s 等证书申请完（首次申请 Let's Encrypt 大约 5-15s）
        tokio::time::sleep(tokio::time::Duration::from_secs(30)).await;

        let mut ticker = tokio::time::interval(tokio::time::Duration::from_secs(60));
        let mut last_cert_hash: u64 = 0;
        let mut last_key_hash: u64 = 0;

        loop {
            ticker.tick().await;

            // certon FileStorage 默认结构: <cache_dir>/<domain>/cert.pem + key.pem
            let domain_dir = cache_dir.join(&domain);
            let cert_src = domain_dir.join("cert.pem");
            let key_src  = domain_dir.join("key.pem");

            // 如果 certon 存的不是 PEM 而是 DER，我们还要加 der→pem 转换
            // 但 certon 默认应该是 PEM（ACME 标准格式）
            let (cert_bytes, key_bytes) = match (std::fs::read(&cert_src), std::fs::read(&key_src)) {
                (Ok(c), Ok(k)) => (c, k),
                _ => continue,  // 证书还没存好或路径不对
            };

            let new_cert_hash = fnv1a(&cert_bytes);
            let new_key_hash  = fnv1a(&key_bytes);

            if new_cert_hash != last_cert_hash || new_key_hash != last_key_hash {
                tracing::info!(
                    domain = %domain,
                    "🔄 certon 检测到证书变更 —— 同步到 ../tls/ + 通知 nsbcd"
                );

                // 同步到 ../tls/cert.pem + key.pem（nsbcd SIP TLS + WebRTC WSS 共用）
                if let Some(dir) = cert_path.parent() {
                    let _ = std::fs::create_dir_all(dir);
                }
                let _ = std::fs::write(&cert_path, &cert_bytes);
                let _ = std::fs::write(&key_path, &key_bytes);

                #[cfg(unix)]
                {
                    use std::os::unix::fs::PermissionsExt;
                    let _ = std::fs::set_permissions(&key_path, std::fs::Permissions::from_mode(0o600));
                }

                last_cert_hash = new_cert_hash;
                last_key_hash  = new_key_hash;

                notify_nsbcd_reload();
            }
        }
    });

    // keep task alive
    let _ = pem_bridge_task;

    Ok(TlsSource::Public { rustls_config: Arc::new(rustls_config) })
}

fn fnv1a(data: &[u8]) -> u64 {
    let mut h: u64 = 0xcbf29ce484222325;
    for &b in data {
        h ^= b as u64;
        h = h.wrapping_mul(0x100000001b3);
    }
    h
}

// =====================================================================
// util
// =====================================================================

/// 证书路径 resolver（绝对/相对兼容）供 CLI 子命令 `generate-cert` 用
#[allow(dead_code)]
pub fn resolve_cert_path(p: &Path) -> PathBuf {
    if p.is_absolute() { p.to_path_buf() }
    else {
        std::env::current_dir()
            .unwrap_or_else(|_| PathBuf::from("."))
            .join(p)
    }
}

/// 证书续期成功后通知 nsbcd 同步刷新 TLS ctx（SIP TLS + WebRTC WSS）。
/// 实现：读 /var/run/nsbc/nsbcd.pid → `kill -HUP <pid>`
fn notify_nsbcd_reload()
{
    let pid_path = "/var/run/nsbc/nsbcd.pid";
    let pid_str = match std::fs::read_to_string(pid_path) {
        Ok(s) => s.trim().to_string(),
        Err(e) => {
            tracing::warn!(pid_path, error = %e, "cannot read nsbcd pid — TLS reload in nsbcd skipped");
            return;
        }
    };
    let pid: &str = &pid_str;
    match std::process::Command::new("kill").args(["-HUP", pid]).status() {
        Ok(st) if st.success() => {
            tracing::info!(pid, "nsbcd SIGHUP sent — nsbcd will reload TLS certs");
        }
        Ok(st) => {
            tracing::warn!(pid, ?st, "kill -HUP failed (non-zero exit)");
        }
        Err(e) => {
            tracing::warn!(pid, error = %e, "kill -HUP failed (no kill binary?)");
        }
    }
}
