/* 内建模块汇总表（REQ-ARCH-010/011）。
 * 权威源：此处手写完整数组，永远包含所有必需核心模块。
 * configure 生成 builtin_modules.gen.inc 仅用于 mod.load 路径，
 * builtin.c 不再 include 它 —— 避免 configure 扫描漏模块导致 init 失败。
 *
 * 运行时模块启停：nsbc.yaml 里的 modules.enable / modules.disable 段——
 *   REQUIRED 模块禁止 disable（fail-fast）；非 REQUIRED 可以 disable，
 *   init_level 会跳过 ST_DISABLED 状态的模块。 */
#include "module.h"

/* 架构硬约束守卫：NSBC 核心 nsbcd 不访问 DB（SQLite/MySQL/PostgreSQL 全在 Rust Web 边车侧）。
 * 以下 dbal 模块被排除；define NSBC_NO_DB_MODULE=1 时跳过它。 */
#ifndef NSBC_NO_DB_MODULE
extern const nsbc_module_t nsbcm_dbal;
#endif
extern const nsbc_module_t nsbcm_signaling;
extern const nsbc_module_t nsbcm_oam;
extern const nsbc_module_t nsbcm_ha;
extern const nsbc_module_t nsbcm_hep;
extern const nsbc_module_t nsbcm_endpoints;
extern const nsbc_module_t nsbcm_outreg;
extern const nsbc_module_t nsbcm_dialplan;
extern const nsbc_module_t nsbcm_billing;
extern const nsbc_module_t nsbcm_recording;
extern const nsbc_module_t nsbcm_agent_pool;
extern const nsbc_module_t nsbcm_queue;
extern const nsbc_module_t nsbcm_applications;
extern const nsbc_module_t nsbcm_security;

const nsbc_module_t *const nsbc_builtin_modules[] = {
#ifndef NSBC_NO_DB_MODULE
    &nsbcm_dbal,        /* EARLY: db 抽象层自检 */
#endif
    &nsbcm_endpoints,   /* WORKER_READY: HA1 查询 (REQUIRED) */
    &nsbcm_agent_pool,  /* WORKER_READY: ACD 座席池 (REQUIRED) */
    &nsbcm_outreg,      /* WORKER_READY: 外呼注册 */
    &nsbcm_dialplan,    /* WORKER_READY: 拨号规则 */
    &nsbcm_queue,       /* WORKER_READY: ACD 队列 (REQUIRED, 依赖 agent_pool) */
    &nsbcm_hep,         /* FINAL: HEPv3 探针 (CALL_PATH) */
    &nsbcm_signaling,   /* FINAL: SIP B2BUA 核心 (REQUIRED | CALL_PATH) */
    &nsbcm_oam,         /* FINAL: 运维控制 (REQUIRED, 必须早于 security/billing — cmd_reg 在 init 创建) */
    &nsbcm_ha,          /* FINAL: 高可用 */
    &nsbcm_recording,   /* FINAL: 录音 */
    &nsbcm_billing,     /* FINAL: CDR / 计费 (依赖 oam cmd_reg) */
    &nsbcm_applications,/* FINAL: 拨号应用 */
    &nsbcm_security,    /* FINAL: 安全中心 (依赖 oam cmd_reg) */
    (const nsbc_module_t *)0 /* sentinel */
};

/* 给 signaling.c / b2bua.c 等外部调用者拿到模块表的 API */
const nsbc_module_t *const *nsbc_builtin_modules_table(size_t *out_count)
{
    if (out_count) {
        size_t n = 0;
        for (const nsbc_module_t *const *p = nsbc_builtin_modules; *p; p++) n++;
        *out_count = n;
    }
    return nsbc_builtin_modules;
}

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
