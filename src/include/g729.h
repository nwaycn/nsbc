#ifndef NSBC_G729_H
#define NSBC_G729_H

/* G.729 (ITU-T G.729 Annex B) 帧编解码器
 *
 * 依赖 bcg729 库（Belledonne 开源实现）：
 *   make NSBC_WITH_BCG729=1   → 链接真实 bcg729，全功能启用
 *   默认（NSBC_WITH_BCG729=0）→ 编译 stub 路径，codec 注册成功但 init 返回 NOTSUP，
 *   不影响整个工程编译/运行。
 *
 * 帧参数：10ms = 80 samples @ 8kHz = 10 bytes code stream
 */

#ifdef __cplusplus
extern "C" {
#endif

/* 注册 G729 ops（默认 PT=18，Annex B 开）到 codec_registry
 * 无 bcg729 时仍能注册成功，codec_ops_t 存在但 init 返回 NOTSUP */
int g729_register(void);

#ifdef __cplusplus
}
#endif
#endif /* NSBC_G729_H */

/*
    Author:       李浩 (Li Hao)
    Company:      上海宁卫信息技术有限公司 (Shanghai Nway Co., Ltd.)
    Email:        1354608370@qq.com
    Licensed:     Apache License 2.0
    Copyright:    2024-2026
*/
