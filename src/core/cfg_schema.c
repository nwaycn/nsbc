#include "cfg_schema.h"

/* REQ-CFG-004：核心 Schema（P0 键集合）。
 * flags：默认 0=黑名单（变更需重启）；CGF_HOT=白名单热加载；
 *        CGF_SECRET=敏感；CGF_PROM_MIGRATE=仅先开后关（指标迁移期）。 */
static const cfg_desc_t k_core[] = {
    /* ---- REQ-CFG-006：配置 schema 版本（顶层键）。默认 "1.0" 兜底无该
     * 字段的首版文件；flags=0（黑名单）：禁止运行时改写，跨版本升级
     * 只能走离线 config migrate（reload 改它必被 E_NEED_RESTART 拒绝）。 */
    {"config_version", CGT_STR, CGF_RESTART, "1.0", NULL, 0, 0},

    /* ---- 日志 / OBS ---- */
    {"log.level", CGT_ENUM, CGF_HOT, "info",
     "emerg,alert,crit,err,warn,notice,info,debug", 0, 0},
    {"log.path", CGT_STR, CGF_RESTART, "/var/log/nsbc/nsbc.log", NULL, 0, 0},
    {"log.audit_path", CGT_STR, CGF_RESTART, "/var/log/nsbc/audit.log", NULL,
     0, 0},
    {"log.ring_slots", CGT_INT, CGF_RESTART, "512", NULL, 128, 8192},

    /* ---- 核心运行时 ---- */
    {"core.workers", CGT_INT, CGF_RESTART, "0", NULL, 0, 256}, /* 0=按 CPU */
    {"core.worker_queue", CGT_INT, CGF_RESTART, "4096", NULL, 256, 1048576},
    {"core.max_calls", CGT_INT, CGF_RESTART, "100000", NULL, 1, 10000000},
    {"core.timer_tick_ms", CGT_INT, CGF_RESTART, "100", NULL, 10, 1000},
    {"core.drain_max_ms", CGT_INT, CGF_HOT, "30000", NULL, 0, 600000},
    {"core.deploy_profile", CGT_ENUM, CGF_RESTART, "standalone-sbc",
     "standalone-sbc,pbx,fusion", 0, 0},

    /* ---- 内存五域（MiB，7.1.5） ---- */
    {"mem.signal_mb", CGT_INT, CGF_RESTART, "256", NULL, 16, 65536},
    {"mem.packet_mb", CGT_INT, CGF_RESTART, "1024", NULL, 16, 65536},
    {"mem.media_mb", CGT_INT, CGF_RESTART, "512", NULL, 16, 65536},
    {"mem.transcode_mb", CGT_INT, CGF_RESTART, "256", NULL, 0, 65536},
    {"mem.record_mb", CGT_INT, CGF_RESTART, "256", NULL, 0, 65536},
    {"mem.hugepage", CGT_BOOL, CGF_RESTART, "false", NULL, 0, 0},
    {"mem.guard_band", CGT_BOOL, CGF_RESTART, "false", NULL, 0, 0},

    /* ---- 通话录音（REQ-REC-001~004，P1） ---- */
    {"recording.enabled", CGT_BOOL, CGF_HOT, "true", NULL, 0, 0},
    {"recording.path", CGT_STR, CGF_HOT, "/var/lib/nsbc/recordings", NULL, 0, 0},
    {"recording.policy", CGT_ENUM, CGF_HOT, "manual",
     "always,manual,never,dialplan", 0, 0},
    {"recording.mode", CGT_ENUM, CGF_RESTART, "mixed",
     "single,mixed,separate", 0, 0},
    {"recording.format", CGT_ENUM, CGF_RESTART, "wav",
     "wav,opus", 0, 0},
    {"recording.max_concurrent", CGT_INT, CGF_HOT, "128", NULL, 1, 1024},
    {"recording.max_mb_per_file", CGT_INT, CGF_HOT, "512", NULL, 16, 65536},
    {"recording.encrypt", CGT_BOOL, CGF_RESTART, "false", NULL, 0, 0},

    /* ---- 出局注册（REQ-SIP-OUT-001，P1） ---- */
    {"outbound.register_trunks", CGT_STR, CGF_HOT, "", NULL, 0, 0},

    /* ---- 网络 ---- */
    {"net.reuseport", CGT_BOOL, CGF_RESTART, "true", NULL, 0, 0},
    {"net.rcvbuf_kb", CGT_INT, CGF_HOT, "4096", NULL, 64, 1048576},
    {"net.sndbuf_kb", CGT_INT, CGF_HOT, "4096", NULL, 64, 1048576},
    {"net.sip_listen_ip", CGT_STR, CGF_RESTART, "0.0.0.0", NULL, 0, 0},
    {"net.sip_udp_port", CGT_INT, CGF_RESTART, "5060", NULL, 1, 65535},
    {"net.sip_tcp_port", CGT_INT, CGF_RESTART, "5060", NULL, 1, 65535},
    {"net.sip_tls_port", CGT_INT, CGF_RESTART, "5061", NULL, 1, 65535},
    {"net.sip_max_msg", CGT_INT, CGF_RESTART, "16384", NULL, 2048, 65535},
    {"net.tcp_conn_max", CGT_INT, CGF_HOT, "100000", NULL, 64, 2000000},
    {"net.tls_enabled", CGT_BOOL, CGF_RESTART, "false", NULL, 0, 0},
    {"net.tls_cert", CGT_STR, CGF_RESTART, "/etc/nsbc/tls/cert.pem", NULL, 0,
     0},
    {"net.tls_key", CGT_STR, CGF_SECRET | CGF_RESTART,
     "file:/etc/nsbc/tls/key.pem", NULL, 0, 0},
    {"net.tls_min_version", CGT_ENUM, CGF_RESTART, "1.2", "1.2,1.3", 0, 0},
    {"net.dns_ttl_s", CGT_INT, CGF_HOT, "60", NULL, 0, 3600},

    /* ---- SIP 事务（RFC3261 定时器，单位 ms） ---- */
    {"sip.t1_ms", CGT_INT, CGF_HOT, "500", NULL, 50, 5000},
    {"sip.t2_ms", CGT_INT, CGF_HOT, "4000", NULL, 500, 16000},
    {"sip.t4_ms", CGT_INT, CGF_HOT, "5000", NULL, 500, 20000},
    {"sip.timer_d_ms", CGT_INT, CGF_HOT, "32000", NULL, 1000, 120000},
    {"sip.ring_noanswer_timeout_s", CGT_INT, CGF_HOT, "60", NULL, 5, 3600},
    /* PBX 基础：180/183 ringing 60s 无应答 → 向 A 腿发 480 Temporarily Unavailable */
    /* REQ-SIP-DIV-001 P0：302/380 自动重定向 */
    {"sip.auto_redirect_302", CGT_BOOL, CGF_HOT, "true", NULL, 0, 0},
    {"sip.max_redirects", CGT_INT, CGF_HOT, "5", NULL, 0, 20},
    {"sip.udp_cl_strict", CGT_BOOL, CGF_HOT, "true", NULL, 0, 0},
    {"sip.user_agent", CGT_STR, CGF_HOT, "NSBC/2.5", NULL, 0, 0},
    {"sip.enable_udp", CGT_BOOL, CGF_RESTART, "true", NULL, 0, 0},
    {"sip.enable_tcp", CGT_BOOL, CGF_RESTART, "true", NULL, 0, 0},
    {"sip.realm", CGT_STR, CGF_RESTART, "nsbc.local", NULL, 0, 0},
    {"sip.extern_ip", CGT_STR, CGF_RESTART, "", NULL, 0, 0},
    {"sip.digest_secret", CGT_STR, CGF_SECRET | CGF_RESTART,
     "file:/etc/nsbc/digest.key", NULL, 0, 0},
    /* ---- 分机预填充（P0 静态表；P1 从 DB/API 热加载） ----
     * 格式："user=ha1hex,user=ha1hex,..."；ha1hex 为 32 字符 MD5(user:realm:password) */
    {"sip.extensions", CGT_STR, CGF_RESTART, "", NULL, 0, 0},
    /* ---- Dialplan 规则（P0 简单规则集） ----
     * 单条格式："match_pattern:layer:target[:strip][:add_prefix]"
     * layer: 0=EXT(分机内呼), 1=BIZ(业务号), 2=OUT(出局)
     * 多条用 '|' 分隔；空表示不加载任何规则 */
    {"dialplan.rules", CGT_STR, CGF_RESTART, "", NULL, 0, 0},
    {"media.allow_bypass", CGT_BOOL, CGF_HOT, "false", NULL, 0, 0},
    {"media.trust_private", CGT_BOOL, CGF_HOT, "true", NULL, 0, 0},
    {"registrar.rps_per_aor", CGT_INT, CGF_HOT, "10", NULL, 0, 10000},

    /* ---- RTP/媒体 ---- */
    {"rtp.port_start", CGT_INT, CGF_RESTART, "10000", NULL, 1024, 65000},
    {"rtp.port_end", CGT_INT, CGF_RESTART, "59999", NULL, 1024, 65535},
    {"rtp.latching", CGT_BOOL, CGF_RESTART, "true", NULL, 0, 0},
    {"rtp.dscp", CGT_INT, CGF_HOT, "46", NULL, 0, 63},
    {"rtp.ssrc_grace_ms", CGT_INT, CGF_HOT, "500", NULL, 0, 10000},
    {"rtp.rtcp_interval_ms", CGT_INT, CGF_HOT, "5000", NULL, 500, 60000},
    {"media.default_mode", CGT_ENUM, CGF_HOT, "relay",
     "bypass,relay,transcode", 0, 0},

    /* ---- NAT / ICE / STUN / TURN（REQ-WEBRTC-001） ---- */
    {"nat.stun.enabled", CGT_BOOL, CGF_RESTART, "true", NULL, 0, 0},
    {"nat.stun.port", CGT_INT, CGF_RESTART, "3478", NULL, 1, 65535},

    /* ---- SIP over WebSocket（REQ-WS-001） ---- */
    {"sip.ws.enabled", CGT_BOOL, CGF_RESTART, "true", NULL, 0, 0},
    {"sip.ws.port", CGT_INT, CGF_RESTART, "5062", NULL, 1, 65535},
    {"sip.ws.tls", CGT_BOOL, CGF_RESTART, "true", NULL, 0, 0},

    /* ---- 注册 ---- */
    {"registrar.min_expires", CGT_INT, CGF_HOT, "60", NULL, 10, 3600},
    {"registrar.default_expires", CGT_INT, CGF_HOT, "3600", NULL, 60, 86400},
    {"registrar.max_expires", CGT_INT, CGF_HOT, "3600", NULL, 60, 86400},
    {"registrar.max_contacts", CGT_INT, CGF_HOT, "10", NULL, 1, 20},
    {"registrar.rps_global", CGT_INT, CGF_HOT, "2000", NULL, 0, 1000000},
    {"registrar.rps_per_ip", CGT_INT, CGF_HOT, "100", NULL, 0, 100000},

    /* ---- DB（REQ-DB-001~007） ---- */
    {"db.driver", CGT_ENUM, CGF_RESTART, "sqlite",
     "sqlite,mysql,postgres,none", 0, 0},
    {"db.dsn", CGT_STR, CGF_SECRET | CGF_RESTART,
     "file:/var/lib/nsbc/nsbc.db", NULL, 0, 0},
    {"db.pool_size", CGT_INT, CGF_RESTART, "8", NULL, 1, 256},
    {"db.group_commit_ms", CGT_INT, CGF_HOT, "10", NULL, 1, 100},
    {"db.group_commit_rows", CGT_INT, CGF_HOT, "100", NULL, 1, 1000},

    /* ---- 配置快照与回滚（REQ-CFG-006/008；快照目录随部署注入） ---- */
    {"system.config_snapshot_dir", CGT_STR, CGF_RESTART,
     "/var/lib/nsbc/config-snapshots", NULL, 0, 0},
    {"system.config_snapshot_keep", CGT_INT, CGF_HOT, "20", NULL, 1, 200},

    /* ---- OAM / 指标 ----
     * 三个 *.sock 路径已硬编码 /var/run/nsbc/（代码不再读取），
     * 条目保留仅供 config.export 展示一致，避免误导。 */
    {"oam.metrics_enabled", CGT_BOOL, CGF_HOT, "true", NULL, 0, 0},
    {"oam.metrics_bind", CGT_STR, CGF_HOT | CGF_PROM_MIGRATE,
     "127.0.0.1:9100", NULL, 0, 0},
    {"oam.control_sock", CGT_STR, CGF_RESTART,
     "unix:/var/run/nsbc/control.sock", NULL, 0, 0},
    {"oam.data_sock", CGT_STR, CGF_RESTART,
     "/var/run/nsbc/data.sock", NULL, 0, 0},
    /* 2026-10-20: oam.esl_sock 已暂停（control.sock 提供全部 OAM，
     * data.sock 推事件，esl.sock 从未被实际使用）。 */

    /* ---- HA（P0 主备仲裁；Hitless 为 P1） ---- */
    {"ha.enabled", CGT_BOOL, CGF_RESTART, "false", NULL, 0, 0},
    {"ha.peer", CGT_STR, CGF_RESTART, "", NULL, 0, 0},
    {"ha.heartbeat_ms", CGT_INT, CGF_HOT, "500", NULL, 50, 10000},
    {"ha.failover_ms", CGT_INT, CGF_HOT, "3000", NULL, 100, 60000},

    /* ---- Web 边车（第 14 章，默认关闭） ---- */
    {"web.enabled", CGT_BOOL, CGF_RESTART, "false", NULL, 0, 0},
    {"web.bind", CGT_STR, CGF_RESTART, "127.0.0.1:8443", NULL, 0, 0},
};

int cfg_schema_register_core(cfg_t *c)
{
    return cfg_schema_declare(c, k_core,
                              (unsigned)(sizeof(k_core) / sizeof(k_core[0])));
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
