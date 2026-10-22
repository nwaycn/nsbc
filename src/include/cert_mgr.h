#ifndef NSBC_CERT_MGR_H
#define NSBC_CERT_MGR_H

/* 证书生命周期管理（REQ-SEC-002，P0 证书加载/到期监控；
 * P1 CRL/OCSP 吊销、热轮换）
 *
 * 与 tls_io 的分工：tls_io 负责连接上的 SSL 握手；本模块负责磁盘
 * 证书文件的装载、校验、到期预警、SIGHUP/定时热加载，并把刷新后的
 * tls 上下文推送给监听点（SIP TLS / mTLS 中继 / HTTPS OAM）。
 * 构建未带 NSBC_WITH_OPENSSL：接口返回 NSBC_NOTSUP（同 tls_io 口径）。
 * 分层：net（1）。禁止在本模块做网络侧 OCSP 直连（OCSP stapling 由
 * 对端/网关侧处理，本进程 P1 只做 CRL 文件 + stapling 校验位）。
 */
#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CERT_NAME_MAX  32
#define CERT_PATH_MAX  256
#define CERT_SET_MAX   32

typedef struct {
    char name[CERT_NAME_MAX];
    char cert_pem[CERT_PATH_MAX];
    char key_pem[CERT_PATH_MAX];
    char ca_pem[CERT_PATH_MAX];
    char crl_pem[CERT_PATH_MAX];   /* P1：CRL 文件（空=不校验吊销） */
    int  require_ocsp_staple;      /* P1：要求对端 stapling */
    int  check_peer;
} cert_set_t;

/* 证书上下文刷新回调：证书热加载后，监听者据此重建 SSL_CTX；
 * 新 ctx 就绪前老连接不中断（优雅轮换，REQ-SEC-002）。 */
typedef struct cert_mgr cert_mgr_t;
typedef void (*cert_refresh_cb)(void *ud, const char *name);

int  cert_mgr_create(cert_mgr_t **out);
void cert_mgr_destroy(cert_mgr_t *m);

int  cert_set_add(cert_mgr_t *m, const cert_set_t *s);
int  cert_set_remove(cert_mgr_t *m, const char *name);
const cert_set_t *cert_set_get(cert_mgr_t *m, const char *name);

/* 装载并校验（存在性/密钥匹配/证书链/时间窗）；返回 NOTSUP 表示
 * 当前构建无 OpenSSL；校验失败返回 AUTH，详情经事件槽上报。 */
int  cert_load(const cert_mgr_t *m, const char *name);
int  cert_reload_all(cert_mgr_t *m);

/* 到期检查：返回最小剩余天数（全部 set 的最早 notAfter 距 now_days
 * 基准），出参可空；now_days=0 时粗骨架跳过（由调用方传时间）。 */
int  cert_min_days_left(cert_mgr_t *m, int64_t now_days,
                        char *name_out, size_t cap);

/* 热加载监听（tls_io/HTTPS OAM 装配） */
void cert_on_refresh(cert_mgr_t *m, cert_refresh_cb cb, void *ud);

/* 指纹（SHA-256 冒号十六进制）供 SIP SIPS/TLS pinning 与审计；
 * 无 OpenSSL 构建返回 NOTSUP。out 需 ≥96 字节。 */
int  cert_fingerprint256(const cert_mgr_t *m, const char *name,
                         const char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_CERT_MGR_H */
