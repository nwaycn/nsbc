/* PBX 业务应用（第 3 章）：IVR、语音信箱、振铃组、ACD、Follow-me
 * 以子模块注册，不另开顶层目录（REQ-7.8）。
 *
 * P0：仅模块占位与命令/事件注册点；Dialplan App 执行器已在
 * mod/dialplans 落地。IVR（放音收号/菜单/转接/超时重试，
 * REQ-PBX-IVR-001）、振铃组等在 P1 实现，媒体动作复用 media/play。
 */
#include "module.h"

static int applications_init(struct nsbc_ctx *ctx)
{
    (void)ctx;
    return NSBC_OK;
}

NSBC_MOD0_CAT(applications, 1, MOD_L_FINAL, 0, MOD_CAT_APPLICATION, applications_init, NULL);

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
