# 【更新日报 2026-11-20】NSBC 引入 WebRTC DSP 链 — AI 通话音频质量升级

源于昨天有同学说，能不能把降噪、回声、VAD 误判等加进来？我当时说，回声消除最好还是终端处做，但是喝了场酒，晚上睡觉时一想，AI Agent不就是一个个终端，自然也可以在这里处理了，所以就加了WebRTC DSP。

> 今天两个 commit（`c01f48c` + `3f40066`）合计 **+558 / -161 行**，把 WebRTC 原生的 **AECM 回声消除 / NS 降噪 / AGC 自动增益** 和 **WebRTC VAD 频谱判据** 接入了 AI 对话媒体管道。核心解决 3 个线上问题：回声泄漏、噪声进 ASR、VAD 误判敲击。

---

## 一、改了什么（按模块）

### 1. `src/mod/ai_dialog/ai_dialog.c` — +464 行（主力改动）

**新增 per-leg WebRTC DSP 链**，在 G.711 → PCM16 解码之后、ring buffer 写入之前跑，**两腿（主叫/被叫）各一套独立实例**：

```
RTP → transcode → g711_decode → HPF(去DC+50Hz)
                                ↓
                          AECM 回声消除 ─── (自动喂 farend：我们发给话机的 TTS)
                                ↓
                          NS 降噪 (Mild, 6dB)
                                ↓
                          噪声门 (|sample| < 80 → 归零)   ← 必须放 AGC 之前
                                ↓
                          AGC 自动增益 (adaptive digital, +12dB)
                                ↓
                          VAD 门控 (非语音不写 ring)   ← 200ms pre-roll + 200ms hangover
                                ↓
                          ring buffer → ai_get_rx_audio → ASR
```

**关键设计点**：

| 项目 | 值 | 原因 |
|---|---|---|
| DSP 块长 | 10ms / 80 samples | WebRTC AECM/NS/AGC 统一要求 |
| AECM 远端参考 | 我们发给话机的 TTS PCM16 | ai_send_tx_audio 里自动 BufferFarend，AECM 内部延迟估计器自己搜 |
| NS 模式 | Mild / 6dB | Strong 会压掉轻声，Mild 够用 |
| AGC 模式 | adaptive digital, target -3dBFS, +12dB, limiter=on | 保证用户说话被 ASR 录到足够电平 |
| 噪声门位置 | **AGC 之前** | 实测：NS 后残噪 30~50，放 AGC 之后会被放大到 200+ 嘶声 |
| VAD 门控 | SILENCE 不写 ring | AI agent 拿到的全是语音片段，减少无效 ASR 请求 |
| pre-roll / hangover | 各 200ms | 防丢字头/字尾 |

**生命周期防御**：某一级创建失败只降级该级（不跑 AECM 或 NS），不影响通话。WebRTC 实例全部 NULL 时自动 fail-open 直通。

---

### 2. `src/media/vad_engine.c` — +107 行（VAD 升级）

**引入 WebRTC VAD 作为二级判据**（`common_audio/vad/include/webrtc_vad.h`），解决纯能量 VAD 被敲桌子/敲门这类**宽带瞬态**误判的问题。

FSM 所有状态转换条件现在变成「双判据」：

```
原来：near > threshold × 0.6 且 active_count ≥ 40ms
现在：near > threshold × 0.6 且 active_count ≥ 160ms 且 WebRTC VAD 判为语音
```

具体改动：
- **SILENCE → SPEECH**：加 `wvad_speech` 约束
- **突发 SILENCE → SPEECH**：原来用 `/2` 绕过持续时间门槛 → 改成同 160ms 门槛 + wvad 约束（防止 80ms 敲击瞬态绕过）
- **HESITATION → SPEECH**：加 `wvad_speech + near > threshold × 0.8`（敲击尾巴 rms 16~35 会被压掉）

WebRTC VAD 处理方式：按 10ms 子帧投票（过半才算语音），fail-open（实例创建失败退化为纯能量 VAD）。

---

### 3. `src/include/vad_engine.h` — +6 行（参数调整）

| 常量 | 原值 | 新值 | 原因 |
|---|---|---|---|
| `VAD_PREROLL_SAMPLES` | 960 (120ms) | **1600 (200ms)** | 覆盖 160ms 起判延迟，不丢字头 |
| `VAD_START_ACTIVE_MIN_SAMPLES` | 320 (40ms) | **1280 (160ms)** | 40ms 太短，单次敲击 20~80ms 瞬态会误判；160ms 由 pre-roll 覆盖 |

---

### 4. `src/mod/codecs/g711.c` — +142 / -161 行

**G.711 编解码优化**（未在本 handoff 展开 diff，但 git log 显示同批改动）：
- PCMU↔PCMA 原生解码路径绕 transcode（实测 transcode 链有 PCMU→PCMA→alaw_decode 歪音 bug）
- codec 动态探测：首帧 RX 到达后自动修正 `in_codec`（不再默认 PCMA）
- TX path zero-padding 修复 RTP timestamp 跳跃导致的噼啪噪声

---

## 二、解决了什么问题

| 线上问题 | 之前 | 现在 |
|---|---|---|
| **AI 听到自己 TTS 回声** | 用户说一句，ASR 把 TTS 回复也识别进去了 | AECM 按 farend 参考消掉回声（实测 DIAG 日志 far_avg=200+ 时 post_avg < 10） |
| **环境噪声进 ASR** | 办公室敲击/空调底噪全进 ring buffer | NS Mild 6dB + 噪声门 80 sample 阈值 + VAD 门控 → 静音和瞬态不进 ASR |
| **敲桌子被当成说话** | 纯能量 VAD，敲击 20ms 瞬态 rms 跳 200 → 误判 SPEECH | WebRTC VAD 频谱 GMM 判据，宽带瞬态被判非语音 → 不进 SPEECH |
| **语音检测丢字头** | pre-roll 120ms + START 40ms，160ms 起判延迟里的字头会丢 | pre-roll 200ms 覆盖 160ms 起判 → SPEECH 触发时已补回 200ms 历史 |
| **AGC 放大嘶声** | 残噪 30~50 被 AGC 放大到 200+ | 噪声门放 AGC 之前，±80 直接归零 → AGC 只放大干净语音 |

---

## 三、架构图

```
┌─────────────────────────────────────────────────────────────────┐
│               B2BUA 通话媒体管道（两腿独立）                      │
│                                                                 │
│  ┌─── leg 0 (主叫) ──────────────┐   ┌─── leg 1 (被叫) ────────┐│
│  │                                │   │                         ││
│  │  RTP → G711 decode → PCM16     │   │  RTP → G711 decode → PCM16│
│  │       ↓                        │   │       ↓                 ││
│  │  HPF (DC+50Hz)                 │   │  HPF (DC+50Hz)          ││
│  │       ↓                        │   │       ↓                 ││
│  │  AECM ◄── TTS farend 自动喂    │   │  AECM (远参考空=直通)    ││
│  │       ↓                        │   │       ↓                 ││
│  │  NS (Mild 6dB)                 │   │  NS (Mild 6dB)          ││
│  │       ↓                        │   │       ↓                 ││
│  │  噪声门 (|x|<80→0)             │   │  噪声门                 ││
│  │       ↓                        │   │       ↓                 ││
│  │  AGC (+12dB)                   │   │  AGC                    ││
│  │       ↓                        │   │       ↓                 ││
│  │  VAD 门控 + pre-roll/hangover  │   │  VAD 门控               ││
│  │       ↓                        │   │       ↓                 ││
│  │       └─────────┬──────────────┘   └────────┬────────────────┘│
│                    ↓                           ↓                  │
│              ring buffer (512KB, PCM16 LE 8kHz)                  │
│                    ↓                                             │
│           ai_get_rx_audio → Go agent → ASR/LLM/TTS               │
└─────────────────────────────────────────────────────────────────┘

TTS 注入路径（反方向）：
  Go agent → ai_send_tx_audio(format=1, PCM16) → codec.encode → bridge_send_raw
                                                          ↓
                                               自动 BufferFarend 到 leg 0 AECM
```

---

## 四、编译 & 部署

WebRTC 原生代码来自 `3rd/webrtc/` 目录（aecm.c / agc.c / noise_suppression.c / common_audio/vad）。**首次部署需要**：

```bash
# 1. 拉取 3rd/webrtc 子模块（如果用 git submodule）
cd /home

git clone https://gitee.com/nwaycn/nsbc.git



# 2. 编译（纯 C，无外部依赖）
cd nsbc
./configure
make clean
make -j$(nproc)
```

ai_dialog.c 里用 `extern` 声明 AECM/AGC 符号、只 `#include "noise_suppression.h"`，**不 include aecm.h / agc.h** — 因为那两个头文件带 `static const` 表，`-Wall -Wextra -Werror` 下会触发 `-Wunused-const-variable` 编译失败。

---

## 五、观察 & 回滚

**观察日志**（每腿每秒一条）：

```
DIAG dsp call=0x7f... leg=0 pre_avg=312 aec_avg=89 ns_avg=62 gate_avg=21 post_avg=247 pre_max=1820 post_max=201
DIAG vad  call=0x7f... leg=0 state=2 rms=280 thr=180 near=312.0 far=89.0 above_nf=1 wvad=1 act_cnt=1280 sil_cnt=320
```

关键指标：
- `pre_max >> post_max` → AECM/NS 有效压掉了回声/噪声 ✅
- `wvad=0` 但 `above_nf=1` → 能量判语音但频谱判瞬态，会被压掉 ✅
- `far_blocks=0` → AECM 从未喂过 farend（TTS 没发过 or 是首次通话）

**回滚**：所有 WebRTC 实例都是 fail-open — 创建失败自动跳过该级，功能退化但通话不受影响。要完全关闭，临时注释掉 `ai_dialog.c` 的 `ai_dsp_init()` 调用即可。

---

## 六、文件清单

| 文件 | 改动量 | 说明 |
|---|---|---|
| `src/mod/ai_dialog/ai_dialog.c` | +464 | DSP 链实现（AECM/NS/AGC/噪声门/VAD门控） + farend 自动喂 + DIAG 日志 |
| `src/media/vad_engine.c` | +107 | WebRTC VAD 二级判据 + FSM 状态转换加 wvad 约束 |
| `src/include/vad_engine.h` | +6 | pre-roll 120→200ms，start active 40→160ms |
| `src/mod/codecs/g711.c` | +142/-161 | G.711 编解码路径优化 + TX zero-padding 修 RTP 噪声 |
| `3rd/webrtc/` | — | **外部依赖**：aecm.c / agc.c / noise_suppression.c / common_audio/vad |

---

*NSBC 媒体管道 · 2026-11-20 · commit 3f40066 / c01f48c*
