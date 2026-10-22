#ifndef NSBC_REG_ENHANCE_H
#define NSBC_REG_ENHANCE_H

/* 注册增强（REQ-REG-ENH，P1/P2）
 *
 *  - GRUU（RFC5627，P1）：Contact 的 pub-gruu/temp-gruu 参数签发与
 *    gruu 寻址反查（temp-gruu = H(AOR|secret|周期盐) 截断 base64url，
 *    周期轮换盐以解除长期关联）；
 *  - 第三方注册（RFC6140，P2）：From/AOR 与 To/target-user 不一致的
 *    REGISTER，校验授权关系（第三方授权表，粗骨架白名单）；
 *  - q 参数（RFC3261 §11.3）：Contact 优先级解析，序列化绑定降级；
 *  - 注册持久化（P1）：绑定变更落 DBAL（重启/主备切换后快速恢复，
 *    不等终端重注册）；集群同步（P2）：变更经 ha 复制总线广播；
 *  - 号段粒度授权（P2）：网关账号按号段注册多个 AOR（授权前缀表）；
 *  - 注册风暴防护：主实现已在 registrar.c 三级令牌桶（P0），本模块
 *    只提供配置校验辅助（阈值合法范围 + 倍率换算）。
 * 分层：registrar（3），依赖 db(location_db)/parse(sip_msg)。
 */
#include "nsbc_common.h"
#include "sip_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REGENH_AOR_MAX      96
#define REGENH_SECRET_MAX   64
#define REGENH_GRANT_CAP    4096
#define REGENH_PREFIX_MAX   32
#define REGENH_USER_MAX     64

typedef enum {
    GRUU_NONE = 0,
    GRUU_PUBLIC,
    GRUU_TEMP
} gruu_kind_t;

/* GRUU 配置：secret 为签发密钥（部署生成，不入库明文日志）；
 * salt_epoch_ms 为 temp-gruu 盐轮换起点（管理面周期换盐）。 */
typedef struct {
    char     secret[REGENH_SECRET_MAX];
    uint64_t salt_epoch_ms;
} gruu_cfg_t;

typedef struct reg_enh reg_enh_t;

int  reg_enh_create(const gruu_cfg_t *cfg, reg_enh_t **out);
void reg_enh_destroy(reg_enh_t *e);

/* 为 AOR 签发 GRUU opaque token（base64url 无填充；realm 由调用方拼） */
int  gruu_issue(reg_enh_t *e, const char *aor, gruu_kind_t kind,
                char *out, size_t cap);
/* 反查：gruu opaque → AOR；temp-gruu 在换盐后可能失效（NOENT，
 * 终端需重新订阅/注册） */
int  gruu_resolve(reg_enh_t *e, const char *opaque,
                  char *aor_out, size_t cap, gruu_kind_t *kind_out);

/* q 参数：从 Contact 头参数区解析 q（0.000..1.000 三位小数）。
 * 缺省返回 1000；非法（超界/非数）返回 INVAL。reg 侧按 q 降序选路。 */
int  reg_parse_q(const sip_tok_t *contact_params, int *q_milli);

/* 第三方注册授权（RFC6140）：from_user 代 to_user 注册 */
int  reg3p_grant_add(reg_enh_t *e, const char *tenant,
                     const char *from_user, const char *to_prefix);
int  reg3p_check(reg_enh_t *e, const char *tenant,
                 const char *from_user, const char *to_user);

/* 号段授权：账号 account 允许注册 prefix 号段（多值授权） */
int  reg_range_grant(reg_enh_t *e, const char *tenant,
                     const char *account, const char *prefix);
int  reg_range_check(reg_enh_t *e, const char *tenant,
                     const char *account, const char *aor_user);

/* 风暴阈值校验（均 >0 且 aor<=ip*64<=global 的粗口径）；
 * 非法返回 INVAL，合法返回 OK（不改值；自动倍率在管理面处理）。 */
int  reg_throttle_validate(int ip_cps, int aor_cps, int global_cps);

/* 持久化/集群同步回调（core 装配：dbal upsert / ha replica publish） */
typedef struct {
    int  (*persist)(void *ud, const char *aor, const char *contact,
                    int expires_s, int removed);
    int  (*cluster_sync)(void *ud, const char *aor, const char *contact,
                         int expires_s, int removed);
    void *ud;
} reg_sync_ops_t;
void reg_sync_set_ops(reg_enh_t *e, const reg_sync_ops_t *ops);
int  reg_sync_publish(reg_enh_t *e, const char *aor, const char *contact,
                      int expires_s, int removed);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_REG_ENHANCE_H */
