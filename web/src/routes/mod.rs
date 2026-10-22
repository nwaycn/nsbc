//! 路由聚合（REQ-WEB-004 MVP 子集）。

pub mod auth;
pub mod billing;
pub mod calls;
pub mod cdrs;
pub mod codecs;
pub mod config;
pub mod dialplan_apps;
pub mod dialplan_rules;
pub mod extensions;
pub mod gateways;
pub mod ha_groups;
pub mod ivr_menus;
pub mod metrics;
pub mod queues;
pub mod recordings;
pub mod registrations;
pub mod route_groups;
pub mod security;
pub mod sip_profiles;
pub mod system;
pub mod tenants;
pub mod time_groups;
pub mod transcode;
pub mod trunks;

#[cfg(test)]
mod tests {
    //! 路由 ↔ 核心客户端映射测试（不含鉴权/版本中间件；全链路在 main 测试）。
    use std::collections::BTreeMap;
    use std::net::SocketAddr;
    use std::sync::Arc;

    use axum::body::Body;
    use axum::http::{Request, StatusCode};
    use axum::Router;
    use tower::ServiceExt;

    use crate::auth::AuthSvc;
    use crate::coreclient::{CoreError, MockCore};
    use crate::{AppState, API_MAJOR};

    fn api_router(canned: BTreeMap<String, Result<String, CoreError>>)
        -> (Router, Arc<std::sync::Mutex<Vec<String>>>)
    {
        let seen = Arc::new(std::sync::Mutex::new(Vec::new()));
        let core = MockCore { calls: seen.clone(), canned: Arc::new(canned) };
        let st = Arc::new(AppState {
            core,
            auth: Arc::new(AuthSvc::for_test_empty()),
            db: crate::db::Db::new(),
            api_major: API_MAJOR,
            bind: SocketAddr::from(([127, 0, 0, 1], 0)),
            recording_root: std::path::PathBuf::from("/tmp/nsbc-test-recordings"),
        });
        let app = Router::new()
            .merge(super::system::router::<MockCore>())
            .merge(super::config::router::<MockCore>())
            .merge(super::calls::router::<MockCore>())
            .merge(super::registrations::router::<MockCore>())
            .merge(super::metrics::router::<MockCore>())
            .merge(super::cdrs::router::<MockCore>())
            .with_state(st);
        (app, seen)
    }

    fn ok_map() -> BTreeMap<String, Result<String, CoreError>> {
        BTreeMap::from([
            ("version".to_string(), Ok("nsbcd 2.5.5\n".into())),
            ("show calls".to_string(), Ok("active=3 total=10 failed=2\n".into())),
            ("show registrations".to_string(), Ok("registrations=42\n".into())),
            (
                "stats".to_string(),
                Ok("nsbc_up 1\nnsbc_calls_active 3\n".into()),
            ),
        ])
    }

    async fn get(app: &Router, uri: &str) -> (StatusCode, String) {
        let resp = app
            .clone()
            .oneshot(Request::builder().uri(uri).body(Body::empty()).unwrap())
            .await
            .unwrap();
        let status = resp.status();
        let bytes = axum::body::to_bytes(resp.into_body(), 1 << 16).await.unwrap();
        (status, String::from_utf8(bytes.to_vec()).unwrap())
    }

    #[tokio::test]
    async fn read_only_routes_serve_real_core_data() {
        let (app, cmds) = api_router(ok_map());
        let (s, b) = get(&app, "/version").await;
        assert_eq!(s, StatusCode::OK);
        assert!(b.contains("\"core\":\"2.5.5\""), "body={b}");
        let (s, b) = get(&app, "/calls").await;
        assert_eq!(s, StatusCode::OK);
        assert_eq!(b, r#"{"active":3,"total":10,"failed":2}"#);
        let (s, b) = get(&app, "/registrations").await;
        assert_eq!(s, StatusCode::OK);
        assert_eq!(b, r#"{"registrations":42}"#);
        let (s, b) = get(&app, "/metrics").await;
        assert_eq!(s, StatusCode::OK);
        assert!(b.contains(r#""nsbc_up":1.0"#), "body={b}");
        // 每路由确实打到对应控制命令（无伪造/缓存）
        assert_eq!(
            cmds.lock().unwrap().as_slice(),
            ["version", "show calls", "show registrations", "stats"]
        );
    }

    async fn post(app: &Router, uri: &str) -> (StatusCode, String) {
        let resp = app
            .clone()
            .oneshot(
                Request::builder()
                    .method("POST")
                    .uri(uri)
                    .body(Body::empty())
                    .unwrap(),
            )
            .await
            .unwrap();
        let status = resp.status();
        let bytes = axum::body::to_bytes(resp.into_body(), 1 << 16).await.unwrap();
        (status, String::from_utf8(bytes.to_vec()).unwrap())
    }

    #[tokio::test]
    async fn hangup_success_notfound_and_invalid() {
        let canned = BTreeMap::from([
            ("hangup:c1".to_string(), Ok("OK hungup c1\n".into())),
            ("hangup:gone".to_string(), Err(CoreError::NotFound("x".into()))),
            // 无 "hangup:other" 也无通用 "hangup" → mock 回 Protocol → 502
        ]);
        let (app, cmds) = api_router(canned);
        let (s, b) = post(&app, "/calls/c1/hangup").await;
        assert_eq!(s, StatusCode::OK);
        // serde_json 未启用 preserve_order，键按字典序输出
        assert_eq!(b, r#"{"call_id":"c1","status":"hungup"}"#);
        let (s, b) = post(&app, "/calls/gone/hangup").await;
        assert_eq!(s, StatusCode::NOT_FOUND);
        assert!(b.contains("CALL_NOT_FOUND"), "body={b}");
        let (s, _b) = post(&app, "/calls/other/hangup").await;
        assert_eq!(s, StatusCode::BAD_GATEWAY);
        // 超长 id（>=128）在路由层直接 400，不发控制命令
        let longid = "a".repeat(128);
        let (s, _b) = post(&app, &format!("/calls/{longid}/hangup")).await;
        assert_eq!(s, StatusCode::BAD_REQUEST);
        assert_eq!(
            cmds.lock().unwrap().as_slice(),
            ["hangup c1", "hangup gone", "hangup other"]
        );
    }

    #[tokio::test]
    async fn channels_and_call_detail_routes_map_commands() {
        let canned = BTreeMap::from([
            (
                "show channels".to_string(),
                Ok("call a_callid=a@h a_state=CONFIRMED b_callid=b@h b_state=EARLY\n\
channels=1 truncated=0\n".into()),
            ),
            (
                "uuid_dump:c1".to_string(),
                Ok("call a_callid=c1 a_state=CONFIRMED b_callid=c1 b_state=CONFIRMED\n\
leg 0 state=CONFIRMED proto=udp reliable=0 cseq=5 routes=0\n\
leg 0 tags local=t0 remote=t1\n\
leg 0 media mode=UNAVAILABLE gate=UNAVAILABLE ssrc_in=UNAVAILABLE \
ssrc_out=UNAVAILABLE vq=UNAVAILABLE\n".into()),
            ),
            (
                "uuid_dump:gone".to_string(),
                Err(CoreError::NotFound("x".into())),
            ),
        ]);
        let (app, cmds) = api_router(canned);
        let (s, b) = get(&app, "/channels").await;
        assert_eq!(s, StatusCode::OK);
        assert!(b.contains(r#""a_call_id":"a@h""#), "body={b}");
        assert!(b.contains(r#""truncated":false"#), "body={b}");
        let (s, b) = get(&app, "/calls/c1").await;
        assert_eq!(s, StatusCode::OK);
        assert!(b.contains(r#""state":"CONFIRMED""#), "body={b}");
        // P0 媒体面无采集来源：字段为 null（非估算值）
        assert!(b.contains(r#""mode":null"#), "body={b}");
        // 已结束/不存在 → 404
        let (s, b) = get(&app, "/calls/gone").await;
        assert_eq!(s, StatusCode::NOT_FOUND);
        assert!(b.contains("CALL_NOT_FOUND"), "body={b}");
        // 非法字符（空格/控制字符）与超长 → 400，不发控制命令
        let (s, _) = get(&app, "/calls/bad%20id").await;
        assert_eq!(s, StatusCode::BAD_REQUEST);
        let (s, _) = get(&app, &format!("/calls/{}", "a".repeat(128))).await;
        assert_eq!(s, StatusCode::BAD_REQUEST);
        assert_eq!(
            cmds.lock().unwrap().as_slice(),
            ["show channels", "uuid_dump c1", "uuid_dump gone"]
        );
    }

    #[tokio::test]
    async fn config_key_route_maps_command_and_redacts() {
        let canned = BTreeMap::from([
            (
                "config.get:log.level".to_string(),
                Ok("config key=log.level layer=system secret=0\n6\n".into()),
            ),
            (
                "config.get:db.password".to_string(),
                Ok("config key=db.password layer=system secret=1\n****\n".into()),
            ),
            (
                "config.get:missing.key".to_string(),
                Err(CoreError::NotFound("x".into())),
            ),
        ]);
        let (app, cmds) = api_router(canned);
        let (s, b) = get(&app, "/config/log.level").await;
        assert_eq!(s, StatusCode::OK);
        assert!(b.contains(r#""key":"log.level""#), "body={b}");
        assert!(b.contains(r#""value":"6""#), "body={b}");
        // 敏感键：value=null、secret=true（**** 不出边车）
        let (s, b) = get(&app, "/config/db.password").await;
        assert_eq!(s, StatusCode::OK);
        assert!(b.contains(r#""secret":true"#), "body={b}");
        assert!(b.contains(r#""value":null"#), "body={b}");
        assert!(!b.contains("****"), "body={b}");
        // 不存在 → 404
        let (s, b) = get(&app, "/config/missing.key").await;
        assert_eq!(s, StatusCode::NOT_FOUND);
        assert!(b.contains("CONFIG"), "body={b}");
        // 非法键（空格/点起/连续点/超长）→ 400，不发命令
        for bad in ["/config/bad%20key", "/config/.leading",
                    "/config/a..b", &format!("/config/{}", "k".repeat(128))]
        {
            assert_eq!(get(&app, bad).await.0, StatusCode::BAD_REQUEST, "bad={bad}");
        }
        assert_eq!(
            cmds.lock().unwrap().as_slice(),
            [
                "config.get log.level",
                "config.get db.password",
                "config.get missing.key"
            ]
        );
    }

    #[tokio::test]
    async fn config_export_route_proxies_snapshot_and_redacts() {
        let canned = BTreeMap::from([(
            "config.export".to_string(),
            Ok("config-export keys=2 redact=1\n\
                log.level: 6\n\
                db.dsn: ****\n"
                .into()),
        )]);
        let (app, cmds) = api_router(canned);
        let (s, b) = get(&app, "/config").await;
        assert_eq!(s, StatusCode::OK);
        assert!(b.contains(r#""redacted":true"#), "body={b}");
        assert!(b.contains(r#""key":"log.level""#), "body={b}");
        assert!(b.contains(r#""value":"6""#), "body={b}");
        // 敏感项：value=null/secret=true，**** 不出边车
        assert!(b.contains(r#""key":"db.dsn""#), "body={b}");
        assert!(b.contains(r#""secret":true"#), "body={b}");
        assert!(!b.contains("****"), "body={b}");
        assert_eq!(cmds.lock().unwrap().as_slice(), ["config.export"]);
    }

    #[tokio::test]
    async fn core_down_maps_502_timeout_maps_504() {
        let canned = BTreeMap::from([
            (
                "show calls".to_string(),
                Err(CoreError::Connect("ECONNREFUSED".into())),
            ),
            ("show registrations".to_string(), Err(CoreError::Timeout)),
        ]);
        let (app, _) = api_router(canned);
        let (s, b) = get(&app, "/calls").await;
        assert_eq!(s, StatusCode::BAD_GATEWAY);
        assert!(b.contains("CORE_UNAVAILABLE"), "body={b}");
        let (s, b) = get(&app, "/registrations").await;
        assert_eq!(s, StatusCode::GATEWAY_TIMEOUT);
        assert!(b.contains("CORE_TIMEOUT"), "body={b}");
    }
}
