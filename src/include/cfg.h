#ifndef NSBC_CFG_H
#define NSBC_CFG_H

/* 配置体系（第 13 章，REQ-CFG-001~013）
 *
 * 六层（REQ-CFG-001）：内置默认 < 系统文件 < 环境变量 < Profile
 *                     < 运行时 < 会话级；高优先级覆盖，来源可追溯。
 * 格式：YAML 1.2（P0 内置覆盖 NSBC 配置所用子集的缩进解析器：
 *       map/标量/列表/注释/include；完整 YAML 1.2 由后续 libyaml 驱动替换，
 *       解析接口不变）；include 深度 ≤3、禁环、禁符号链接逃逸（REQ-CFG-002）。
 * 事务：解析进 pending → 静态/动态校验 → 黑名单差异检查 → 原子发布，
 *       任一失败整体回滚（REQ-CFG-008）。
 * 敏感字段：plain:/file:/enc: 三形式，AES-256-GCM（REQ-CFG-009）。
 *
 * 说明：P0 读侧用 pthread_rwlock 保证发布原子性；白名单热更的无锁读
 * （RCU 宽限期回收）在 P1 随配置查询缓存（REQ-DB-006）一起落地。
 */

#include <stdint.h>
#include <stddef.h>
#include "nsbc_common.h"

typedef enum {
    CFG_L_DEFAULT = 0,
    CFG_L_SYSTEM = 1,
    CFG_L_ENV = 2,
    CFG_L_PROFILE = 3,
    CFG_L_RUNTIME = 4,
    CFG_L_SESSION = 5,
    CFG_L__COUNT
} cfg_layer_t;

typedef enum {
    CGT_ANY = 0,
    CGT_STR = 1,
    CGT_INT = 2,
    CGT_BOOL = 3,
    CGT_ENUM = 4
} cfg_type_t;

enum {
    CGF_NONE = 0,
    CGF_RESTART = 0x01, /* 黑名单：变更需重启/Graceful Restart */
    CGF_HOT = 0x02,     /* 白名单：可热加载 */
    CGF_SECRET = 0x04,  /* 敏感字段：show 默认脱敏 */
    CGF_PROM_MIGRATE = 0x08 /* 唯一例外：先开后关（REQ-CFG-007） */
};

typedef struct {
    const char *path; /* 点分路径，如 net.sip_listen_port */
    cfg_type_t type;
    unsigned flags;
    const char *dflt;
    const char *enumv; /* CGT_ENUM：逗号分隔合法值 */
    long min_v, max_v;
} cfg_desc_t;

typedef struct cfg cfg_t;

typedef enum {
    CFG_EV_RELOAD_SUCCESS = 0,
    CFG_EV_RELOAD_FAILED,
    CFG_EV_ROLLBACK,
    CFG_EV_CHANGED
} cfg_event_t;

typedef void (*cfg_event_fn)(cfg_event_t ev, const char *detail, void *ud);

int cfg_create(cfg_t **out, int strict_unknown_keys, int allow_plain_secret);
void cfg_destroy(cfg_t *c);

/* 模块/框架登记 Schema 描述符（描述符表需静态存续） */
int cfg_schema_declare(cfg_t *c, const cfg_desc_t *desc, unsigned n);
int cfg_load_defaults(cfg_t *c);                 /* 第①层：描述符默认值 */
int cfg_load_file(cfg_t *c, const char *path);   /* 第②层（含 include） */
int cfg_apply_env(cfg_t *c, const char *prefix); /* 第③层：默认前缀 NSBC_ */
int cfg_set(cfg_t *c, cfg_layer_t layer, const char *path, const char *value);

/* 运行时单键变更提案（REQ-CFG-011 config.set/config.validate 同源校验链）：
 * apply=0 仅 dry-run 校验（config.validate），不写不发事件；
 * apply=1 校验通过后写 CFG_L_RUNTIME 并发布 CFG_EV_CHANGED（detail=键名）。
 * 与现生效值相同视为幂等成功（*changed=0），不写不发事件。
 * 拒绝口径：未知键 NSBC_NOENT；敏感键/黑名单键 NSBC_PERM_DENIED
 * （*reason 分别为 "secret"/"E_NEED_RESTART"）；类型/范围/枚举非法
 * NSBC_INVAL（"bad-value"）。reason 可为 NULL，changed 可为 NULL。
 * 事件在层写锁释放后回调，与 cfg_reload_file 同口径。 */
int cfg_runtime_propose(cfg_t *c, const char *path, const char *value,
                        int apply, const char **reason, int *changed);

/* 事务化重载系统文件：完整走 解析→校验→黑名单→发布；
 * 失败返回错误码，*bad_key（静态串）给出首个黑名单/非法键。 */
int cfg_reload_file(cfg_t *c, const char *path, const char **bad_key);

/* 启动期快照落盘（REQ-CFG-006）：脱敏生效视图原子写入
 * system.config_snapshot_dir/<UTC时间戳>.yaml（tmp+rename，0600），按
 * system.config_snapshot_keep 留最近 N 份。快照仅脱敏值（敏感行 ****）。
 * 目录不可写等返回错误码，调用方按非致命 WARN 处理；out_name 可空。 */
int cfg_snapshot_save(cfg_t *c, char *out_name, size_t outsz);

/* 列举快照 basename（新→旧每行一个）；缓冲不足 NSBC_NOSPC。 */
int cfg_snapshot_list(cfg_t *c, char *out, size_t outsz, int *count);

/* 回滚（REQ-CFG-008，L1）：id 空=最新份；非空须白名单字符集+.yaml
 * （禁穿越）。平铺解析→校验→黑名单 diff（与 reload 同口径）→原子换
 * SYSTEM 层→清空 RUNTIME 覆盖→发 CFG_EV_ROLLBACK(detail=basename)。
 * 敏感行 **** 不回写，以当前生效值内存 carry。无快照 NSBC_NOENT，
 * id 非法 NSBC_INVAL。 */
int cfg_rollback(cfg_t *c, const char *id, char *out_name, size_t outsz);

/* REQ-CFG-006：配置 schema 版本（顶层键 config_version，独立于文档版本，
 * 按 "主.次" 语义化递增）。CFG_SCHEMA_VERSION 为本构建支持的结构版本。
 * 缺失语义由 schema 默认值 "1.0" 兜底（首版无该字段的文件等价 1.0）。 */
#define CFG_SCHEMA_VERSION "1.0"

/* "M.N" 纯数字版本解析（M/N 为 0..999 十进制）；格式非法 NSBC_INVAL。 */
int cfg_version_parse(const char *s, int *major, int *minor);

/* 启动闸门：have 与 CFG_SCHEMA_VERSION 比较。
 * NSBC_OK：主版本相同；*newer_minor=1 表示文件次版本高于本构建（调用方
 * 记 WARN 后可继续，次版本向后兼容）；主版本不同 NSBC_PERM_DENIED
 * （调用方拒绝启动并提示 migrate 命令）；格式错 NSBC_INVAL。
 * newer_minor 可空；have 不得为 NULL（schema 默认值保证非空）。 */
int cfg_version_check(const char *have, int *newer_minor);

/* 离线迁移配置文件（REQ-CFG-006）：path 从 from 结构迁到 to。
 * 仅支持登记过的迁移路径（首版只有 1.0→1.0：幂等补/改顶层版本行，
 * 无结构变换；未来版本在此登记变换）。步骤：
 *   1) 校验 from/to 格式与迁移路径（未知路径 NSBC_NOTSUP）；
 *   2) 原文件字节备份入快照目录 migrate-<UTC时间戳>-<basename>.yaml
 *      （与启动快照同目录、独立命名：不进 config.snapshots/rollback
 *      列表，也不受 keep 淘汰；路径非法 NSBC_INVAL，文件不存在 NOENT）；
 *   3) 文件实际顶层版本必须等于 --from（字段缺失只允许 from=1.0），
 *      否则 NSBC_INVAL，防对错误版本误操作；
 *   4) tmp+rename 原子替换原文件后，用同一 strict schema 做静态解析
 *      校验；校验失败自动用备份恢复原文件并返回解析错误码（PROTO 等）。
 * 只处理主文件；include 文件不迁移（版本号只存在于顶层主文件）。
 * backup_out 可空，输出备份 basename。 */
int cfg_migrate_file(cfg_t *c, const char *path, const char *from,
                     const char *to, char *backup_out, size_t bsz);

void cfg_set_event_cb(cfg_t *c, cfg_event_fn fn, void *ud);

/* 读取（六层自上而下命中）；layer 非 NULL 输出命中层 */
int cfg_get_str(cfg_t *c, const char *path, const char **out,
                cfg_layer_t *layer);
int cfg_get_int(cfg_t *c, const char *path, long *out, cfg_layer_t *layer);
int cfg_get_bool(cfg_t *c, const char *path, int *out, cfg_layer_t *layer);

const char *cfg_layer_name(cfg_layer_t l);
/* show：敏感键脱敏（****）；secret=1 且已 L2 授权时由调用方审计后解密 */
int cfg_show(cfg_t *c, const char *path, char *out, size_t outsz,
             int reveal);

/* 敏感值解析：plain:/file:/enc: → 明文写入 out（调用方负责用完清零）。
 * 生产策略 allow_plain_secret=0 时遇 plain: 返回 NSBC_PERM_DENIED。 */
int cfg_get_secret(cfg_t *c, const char *path, char *out, size_t outsz);

/* 导出（脱敏）快照（REQ-CFG-006 快照 / REQ-CFG-013 export --redact）。
 * 输出首行为 '#' 注释，其后每键一行 "<dotted.path>: <value>\n"；
 * 缓冲装不下时返回 NSBC_NOSPC（调用方换更大缓冲重试）。 */
int cfg_export_yaml(cfg_t *c, char *out, size_t outsz, int redact);

/* 阶段二动态校验（REQ-CFG-005）：共享段后端三级回退探测 */
int cfg_dynamic_precheck(cfg_t *c, const char **failed_item);

/* 相对路径 resolve（相对于 YAML 文件所在目录）。
 * 自动识别前缀 "file:" / "unix:" —— 只 resolve 后面的路径部分。
 * src 为绝对路径或 cfg_dir 未设置时原样拷贝。
 * 结果写入 dst（dstsz ≥ strlen(src)+PATH_MAX，调用方保证）。 */
void cfg_resolve_path(cfg_t *c, const char *src, char *dst, size_t dstsz);

/* REQ-ARCH-010：modules 段读取。
 * 从 YAML 根的 modules.disable / modules.enable 两个 N_LIST 里取字符串，
 * 分别拷到 out_disables[] / out_enables[]（每项 malloc，调用方 free）。
 * enable 为空（模块全编进来）时 *out_en = 0 → "除 disable 外全 enable"。
 * 函数名语义：nsbc.yaml 里没有 modules 段时返回 NSBC_NOENT。 */
int cfg_get_module_list(cfg_t *c, char ***out_disables, int *out_ndis,
                        char ***out_enables, int *out_nen);

#endif /* NSBC_CFG_H */
