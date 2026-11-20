# WebRTC Audio Processing (vendored)

裁剪自 google/webrtc 的**独立可编译 DSP 模块**，只挑了 NSBC 需要的：

| 文件 | 功能 | 体积 |
|------|------|------|
| `aecm.c/h` | 回声消除 (Echo Cancellation) | ~178KB / 52KB |
| `noise_suppression.c/h` | 噪声抑制 | ~80KB / 12KB |
| `agc.c/h` | 自动增益控制 (Analog + Digital) | ~80KB / 16KB |
| `common_audio/vad/` | VAD GMM（人声/非人声判定） | 5 源文件 ~60KB |
| `common_audio/signal_processing/` | SPL（内联除法/能量/FFT 辅助） | 11 源文件 ~80KB |

## 来源

- 上游：`google/webrtc` —— `modules/audio_processing/` 目录下的 legacy 实现
- 许可证：BSD 3-Clause（见 LICENSE）
- 裁剪方式：从 webrtc-src/ 原始位置挑出**自包含**的 AECM + NS + AGC，其中 AECM 内部自带 DelayEstimator/BinaryDelayEstimator/RealFFT，无需额外 spl/fft 依赖

## 集成方式

- 路径 `3rd/webrtc/`，和 `3rd/bcg729/`、`3rd/cjson/` 同级
- Makefile 加 `-I3rd/webrtc`、`-DNSBC_WITH_WEBRTC_DSP`，并把所有 `*.c` 编入 nsbcd 核心
- 调用点：`src/media/dsp/media_dsp.c` → `dsp_process()` 内串联 **AEC→NS→VAD→strip→AGC** 管线
- AEC far-end 喂入：`dsp_aec_feed_far()` 在 `ai_send_tx_audio()` 下行 AI TTS PCM16 送 RTP 之前调用
- 编译选项：`-Wno-error -Wno-unused-parameter ...` 抑制 WebRTC 原代码中不致命但触发项目严格告警的问题

## 不在本项目构建中的 sibling 文件

- `dr_wav.h`（WAV 编解码）——独立工具链时按需引用
