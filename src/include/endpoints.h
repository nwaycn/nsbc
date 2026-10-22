#ifndef NSBC_ENDPOINTS_H
#define NSBC_ENDPOINTS_H

#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* === 常量（endpoints.c 本地搬过来，signaling.c 解引用 ep_ext_t 字段需要） === */
#ifndef EP_USER_MAX
#define EP_USER_MAX 64
#endif
#ifndef EP_HA1_LEN
#define EP_HA1_LEN 32
#endif
#ifndef EP_CLIP_MAX
#define EP_CLIP_MAX 48
#endif

/* === ep_ext_t 完整定义（REQ-PBX-EXT-EXT-001） ===
 * signaling.c resolve_cb / queue 振铃直接解引用 ext_type / trunk_name /
 * external_number / display_number / clip 做路由决策——必须放头文件而非
 * 隐藏在 endpoints.c 内部。
 *
 * 只读语义：调用方不可修改；reload 整表替换后旧指针悬空，但 resolve_cb
 * 是同步单次调用，不会跨 reload 持有，安全。 */
typedef struct ep_ext {
    char user[EP_USER_MAX];
    char ha1[EP_HA1_LEN + 1];
    char realm[64];
    int used;
    int neg;
    int64_t neg_until_ms;
    char clip[EP_CLIP_MAX];
    uint8_t clir;
    int max_contacts;

    /* === 外部分机字段（signaling.c 解引用） === */
    char ext_type[16];        /* "internal" / "external" */
    char external_number[64]; /* 外呼目标号码（E.164） */
    char trunk_name[64];      /* 出局中继名 */
    char display_number[64];  /* 外呼主叫显示（caller_id） */

    struct ep_ext *next;
} ep_ext_t;

/* 外部分机查询：匹配 (user, realm) → 返回只读 ep_ext_t*
 * NULL = 不存在。 */
const ep_ext_t *ep_lookup(const char *user, const char *realm);

/* 从配置字符串预加载分机 HA1 凭证。
 * 格式："user=ha1hex,user=ha1hex,..."；ha1hex 为 32 字符 MD5(user:realm:password)
 * 每条成功加一条；空串或 NULL 不做任何事。 */
int ep_extensions_load(const char *cfg_str);

/* Web DB 全量推送 → 整表替换（REQ-DATA-SVC-003）。
 * JSON 数组：[{"extension":"1001","ha1":"<32hex>","enabled":true,
 *              "clip":"","clir":false,"max_contacts":10}, ...]
 * 两阶段替换（registrar 值拷贝 ha1_ud 指针，容器地址不动，仅换内容）：
 *   [1] 解析 JSON → 临时 store（失败不动线上表，事务性）
 *   [2] 逐 shard 指针交接（锁序：先 tmp 后 g_ep）
 *   [3] 锁外释放旧链
 * 返回 NSBC_OK / 错误码；失败时 out_err 填原因。 */
int ext_table_reload_json(const char *json, char *out_err, size_t err_sz);

#ifdef __cplusplus
}
#endif

#endif
