#ifndef NSBC_MODULE_H
#define NSBC_MODULE_H

#include "nsbc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* REQ-ARCH-010 统一模块框架
 *
 * 所有业务能力（registrar / b2bua / media bridge / IVR / presence …）
 * 都是模块：声明式描述符 + 分层 init + 依赖拓扑排序（fail-fast）。
 * REQ-ARCH-011：P0/P1 默认静态链接；dlopen 仅开发环境可选
 * （NSBC_WITH_DLOPEN），呼叫面模块永不 dlopen / 永不卸载。
 */

#define NSBC_MOD_NAMELEN 32
#define NSBC_MOD_ABI 1u
#define NSBC_MOD_MAX_DEPS 16

typedef enum {
    MOD_L_EARLY = 0,   /* 仅自检/注册，不依赖运行时 */
    MOD_L_MEM_READY,   /* 内存五域/RCU 就绪 */
    MOD_L_NET_READY,   /* reactor / socket 就绪 */
    MOD_L_WORKER_READY,/* worker 池/时间轮就绪 */
    MOD_L_FINAL,       /* 接收流量前最后一层 */
    MOD_L__COUNT
} mod_level_t;

enum {
    MOD_F_CALL_PATH = 1u << 0, /* 呼叫面：加载后不可卸载 */
    MOD_F_REQUIRED  = 1u << 1, /* 初始化失败即启动失败（fail-fast） */
};

/* 模块能力分类（供 show modules / Web UI 按类型分组显示） */
typedef enum {
    MOD_CAT_UNKNOWN = 0,   /* 未标记（默认） */
    MOD_CAT_CORE,          /* signaling / oam / dbal — 核心框架 */
    MOD_CAT_CODEC,         /* G.711/G.729/H.264/Opus... 编解码注册 */
    MOD_CAT_MEDIA_BUG,     /* media_bug / hep — 媒体旁路/质检 */
    MOD_CAT_ENDPOINT,      /* endpoints / registrar — 注册认证 */
    MOD_CAT_APPLICATION,   /* applications — IVR/Dialplan 应用 */
    MOD_CAT_BILLING,       /* billing — 计费 CDR */
    MOD_CAT_SECURITY,      /* security — 信令安全/过滤 */
    MOD_CAT_OBSERVABILITY, /* event_handlers / metric_reg — 观测 */
    MOD_CAT_CALL_CENTER,   /* callcenter / agent_pool / queue — 呼叫中心 */
    MOD_CAT_AI,            /* ai_dialog / asr_tts — AI 对话 */
    MOD_CAT_OUTBOUND,      /* outreg — 外呼注册 */
    MOD_CAT_UTILITY,       /* cachedb / formats / presence — 基础设施 */
    MOD_CAT__COUNT
} mod_category_t;

/* category → 显示名（供 OAM/Web UI） */
const char *mod_category_name(mod_category_t cat);

struct nsbc_ctx; /* main 装配上下文，前向声明，见 app_ctx.h */

typedef struct nsbc_module {
    const char *name;
    unsigned abi;          /* 必须 == NSBC_MOD_ABI */
    unsigned version;
    const char *const *deps;
    unsigned ndeps;
    mod_level_t level;
    unsigned flags;
    mod_category_t category;
    int (*init)(struct nsbc_ctx *ctx);
    int (*start)(struct nsbc_ctx *ctx); /* 可选：开始收流 */
    void (*stop)(struct nsbc_ctx *ctx); /* 可选：优雅停流 */
    void (*fini)(struct nsbc_ctx *ctx);
} nsbc_module_t;

typedef struct nsbc_modmgr nsbc_modmgr_t;

int modmgr_create(nsbc_modmgr_t **out);
void modmgr_destroy(nsbc_modmgr_t *m);

/* 注册静态模块描述符（主线程启动期，单线程调用） */
int modmgr_register(nsbc_modmgr_t *m, const nsbc_module_t *mod);

/* 注册内建模块表（以 NULL name 结尾） */
int modmgr_register_builtin(nsbc_modmgr_t *m,
                            const nsbc_module_t *const *list);

/* 标记一个已注册模块为 disabled（YAML modules.disable 列表命中时调用）。
 * REQUIRED 模块标记 disabled 会直接返回 NSBC_PERM_DENIED（禁止关掉启动必选模块）。
 * 标记后 init_level 会跳过它（st=ST_DISABLED，不调 init）。
 * 可重复调（幂等）——REQUIRED 校验只在首次标记时做。 */
int modmgr_disable(nsbc_modmgr_t *m, const char *name);

/* 查询模块当前状态（返回 "enabled" / "disabled" / "required-disabled" / "registered" / "missing"） */
const char *modmgr_state_of(nsbc_modmgr_t *m, const char *name);

/* 动态模块加载（REQ-ARCH-010：configure 把非 REQUIRED/CALL_PATH 编为 .so）。
 * 从 dlopen 里找符号 nsbcm_<name>（NSBC_MOD 宏导出）并注册进 modmgr。
 * modmgr_load_all 读文件逐行 .so 路径，批量加载（build/mod.load 格式）。
 * 任何注册失败（ABI 不匹配、dlsym miss、重复名等）返回错误，已加载的 .so 不会卸载
 * ——但整体返回非 0 让调用方决定 fail-fast 还是继续（REQUIRED 语义看模块 flags）。 */
int modmgr_load(nsbc_modmgr_t *m, const char *so_path);
int modmgr_load_all(nsbc_modmgr_t *m, const char *list_path);

/* 按 level 分层拓扑初始化；依赖环/缺依赖/REQUIRED 失败立即 fail-fast */
int modmgr_init_level(nsbc_modmgr_t *m, mod_level_t level,
                      struct nsbc_ctx *ctx);

/* FINAL 层全部 init 成功后，按拓扑序调用 start */
int modmgr_start_all(nsbc_modmgr_t *m, struct nsbc_ctx *ctx);

/* SIGTERM：反序 stop + fini（呼叫模块只 stop，等 drain 后 fini） */
void modmgr_stop_all(nsbc_modmgr_t *m, struct nsbc_ctx *ctx);
void modmgr_fini_all(nsbc_modmgr_t *m, struct nsbc_ctx *ctx);

/* Phase 1 MOD1: 动态模块安全卸载
 * 停止 → fini（模块自己 unregister Registry ops）→ dlclose → 标记为 ST_REG
 * CALL_PATH / REQUIRED 模块拒绝卸载（MOD_F_CALL_PATH | MOD_F_REQUIRED）
 * fail-safe：refcnt>0 时等待超时 → 仍继续 dlclose（宁泄漏不 crash）*/
int modmgr_unload(nsbc_modmgr_t *m, const char *name, struct nsbc_ctx *ctx);

const nsbc_module_t *modmgr_find(nsbc_modmgr_t *m, const char *name);
unsigned modmgr_count(nsbc_modmgr_t *m);

/* 遍历：返回第 idx 个已注册模块（idx < modmgr_count），NULL = 越界。
 * 用于 main.c 的 enable 白名单过滤等场景。 */
const nsbc_module_t *modmgr_get_nth(nsbc_modmgr_t *m, unsigned idx);

/* Phase 5 CLI: 公开模块状态（OAM show modules 显示用）*/
typedef enum {
    MOD_ST_REG = 0,     /* 已注册未 init */
    MOD_ST_DISABLED,    /* 被 YAML modules.disable 标记跳过 */
    MOD_ST_INITED,      /* init 成功（start 可选未调）*/
    MOD_ST_STARTED,     /* start 成功 */
} mod_state_t;

mod_state_t modmgr_get_state_at(nsbc_modmgr_t *m, unsigned idx);

/* Phase 5 CLI：全局 modmgr 指针存取（OAM show modules / show event_bus 命令）*/
void modmgr_set_global(nsbc_modmgr_t *m);
nsbc_modmgr_t *modmgr_get_global(void);

/* 便于模块描述符书写的宏（deps_ 必须是 static const char* 数组）
 * category 默认 MOD_CAT_UNKNOWN；要标记能力类型用 NSBC_MOD_CAT / NSBC_MOD0_CAT */
#define NSBC_MOD(nm, ver, lvl, fl, deps_, init_, start_, stop_, fini_)      \
    const nsbc_module_t nsbcm_##nm __attribute__((visibility("default"))) = {  \
        #nm, NSBC_MOD_ABI, ver, deps_,                                       \
        (unsigned)(sizeof(deps_) / sizeof((deps_)[0])), lvl, fl,              \
        MOD_CAT_UNKNOWN, init_, start_, stop_, fini_}

#define NSBC_MOD0(nm, ver, lvl, fl, init_, fini_)                           \
    const nsbc_module_t nsbcm_##nm __attribute__((visibility("default"))) = {  \
        #nm, NSBC_MOD_ABI, ver, NULL, 0, lvl, fl, MOD_CAT_UNKNOWN,           \
        init_, NULL, NULL, fini_}

/* 显式指定 category 的版本（推荐新模块用这个） */
#define NSBC_MOD_CAT(nm, ver, lvl, fl, cat, deps_, init_, start_, stop_, fini_)  \
    const nsbc_module_t nsbcm_##nm __attribute__((visibility("default"))) = {      \
        #nm, NSBC_MOD_ABI, ver, deps_,                                           \
        (unsigned)(sizeof(deps_) / sizeof((deps_)[0])), lvl, fl, cat,            \
        init_, start_, stop_, fini_}

#define NSBC_MOD0_CAT(nm, ver, lvl, fl, cat, init_, fini_)                       \
    const nsbc_module_t nsbcm_##nm __attribute__((visibility("default"))) = {      \
        #nm, NSBC_MOD_ABI, ver, NULL, 0, lvl, fl, cat, init_, NULL, NULL, fini_}

#ifdef __cplusplus
}
#endif
#endif
