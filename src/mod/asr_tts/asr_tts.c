/* ASR/TTS 引擎适配（P2/P3）：ASR 与 TTS 合为一目录（REQ-7.8）。
 * 仅允许 dlopen 隔离加载，失败不影响核心（REQ-ARCH-011）。占位。 */
#include "module.h"

static int asr_tts_init(struct nsbc_ctx *ctx)
{
    (void)ctx;
    return NSBC_OK;
}

NSBC_MOD0_CAT(asr_tts, 1, MOD_L_FINAL, 0, MOD_CAT_AI, asr_tts_init, NULL);
