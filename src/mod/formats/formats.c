/* 容器格式读写（WAV/OGG/MP3/SLN/raw PCM）：容器与 codec 分离
 * （WAV 内可装 G.729）。P0/P1 仅注册点占位。 */
#include "module.h"

static int formats_init(struct nsbc_ctx *ctx)
{
    (void)ctx;
    return NSBC_OK;
}

NSBC_MOD0_CAT(formats, 1, MOD_L_FINAL, 0, MOD_CAT_UTILITY, formats_init, NULL);

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
