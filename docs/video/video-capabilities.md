# NSBC 视频能力：现状与不足

> 更新：2026-11-24
> 范围：SIP 视频协商 → RTP 收发 → 转码 → 录像 → 抓图 → Web/OAM 入口
> 状态：代码已完成（静态审查通过），**尚未在 10.0.0.80 编译与实网验证**

## 1. 结论速览

| 能力 | H.264 | H.265/HEVC | VP8 | VP9 |
|---|---|---|---|---|
| SDP 协商 | 支持 | 支持（无 fmtp，见 G3） | 支持 | 支持 |
| RTP 打包/解包 | RFC 6184 | RFC 7798 | RFC 7741 | RFC 9628 |
| 关键帧判定 | 支持 | IRAP + VPS/SPS/PPS | 支持 | 支持 |
| 解码 | 支持 | 支持 | 支持 | 支持 |
| 编码 | 支持 | 需 libx265（G2） | 需 libvpx | 需 libvpx |
| 转码到任意视频编码 | 支持 | 支持 | 支持 | 支持 |
| 录像（→ .h264 → mp4） | 支持（reasm 直录） | 支持（转码录制） | 支持（转码录制） | 支持（转码录制） |
| 抓图（JPEG） | 支持 | 不支持（G4） | 不支持（G4） | 不支持（G4） |
| Web 抓图入口 | 支持 | — | — | — |

视频编解码由 `mod_av`（FFmpeg）统一提供；无 FFmpeg 构建时视频 `codec_ops` 仍注册但 `vid_*` 为空，`codec_can_decode/encode` 返回 false，媒体面退化为纯透传。

## 2. 编码注册与优先级

`src/mod/av/av_codec.c`（`AVCODEC_DECLARE_OPS`）：

| codec | clock | default PT | priority | caps |
|---|---|---|---|---|
| H264 | 90000 | 96 | 90 | DEC / ENC |
| H265 | 90000 | 动态 | 85 | DEC / ENC |
| VP8 | 90000 | 动态 | 70 | DEC / ENC |
| VP9 | 90000 | 动态 | 70 | DEC / ENC |
| MJPEG | 90000 | 动态 | 10 | ENC（仅抓图输出） |

内部 RTP 载荷分类（`RTP_PAYLOAD_*`，非 SDP PT）：H264=3、VP8=4、VP9=5、H265=6。
H.265 编码参数：`libx265` + `preset=ultrafast` + `x265-params=tune=zerolatency:repeat-headers=1:keyint=30:min-keyint=30:scenecut=0`；
通用视频编码参数：YUV420P、90000Hz、30fps、800kbps、GOP 30、无 B 帧、`AV_CODEC_FLAG_LOW_DELAY`（见 G8）。

## 3. 转码（video_xcode）

- 触发：两腿视频编码不同（含 H.264/H.265/VP8/VP9 任意组合），由 `bridge_confirm` 规划，`video_xcode_create(src=入腿编码, dst=出腿编码)`。
- 数据流：入腿 RTP → 解包为 Annex-B AU → `vid_decode`（必要时 `sws_scale` 转 YUV420P）→ `vid_encode` → 重新打包发出。
- 同编码不同 PT 不转码，只改 RTP 头 PT（`video_rewrite_*`）。
- 关键帧闸门：等首个关键帧才开始转码（`wait keyframe: drop non-key`）。
- 失败策略：创建失败仅告警并保持透传，不中断通话（H.264→H.265 在无 libx265 时即走此路径）。
- 生命周期：re-INVITE 变更时旧实例进 `vxcode_retired[]`，延迟到 `bridge_release_call` 释放。

## 4. 录像

统一产物：音频 + `.h264` 视频侧车 → 停录时 ffmpeg 合成 mp4（`recording` 模块，始终 `-c:v libx264` 重编码）。

三条链路（按腿独立规划）：

1. **H.264 腿**：`vid_reasm` 重组完整 AU → `rec_feed_video`。`vid_reasm` 在协商到视频即创建，录制会话在喂帧时动态读取，**通话中途开始录像可立即生效**。
2. **VP8/VP9/H.265 腿（独立转码）**：`rec_xcode`（src=该腿编码, dst=H.264）→ AU sink → `rec_feed_video`。
3. **方案 A（复用）**：该腿已有通话转码且目标就是 H.264 时，把录制 AU sink 直接挂到既有 `vxcode`，共用一次解码/编码，不新建 `rec_xcode`。

限制见 G5：链路 2/3 的建立时机绑定 `bridge_confirm`。

## 5. 抓图（snapshot_job）

- 数据源：H.264 腿的完整 AU（`vid_reasm`），解析 SPS/PPS，按 `SNAPSHOT_MAX_FRAMES`（3）取候选帧，过滤黑帧/过小帧（<160×120），择优编码为 JPEG。
- 默认等关键帧超时 2500ms；同一呼叫同一腿最短间隔 2000ms；产物目录 `NSBC_SNAPSHOT_DIR`（默认 `/tmp/nsbc_snapshots`），TTL 默认 3600s 后清理。
- 失败码：`SNAPSHOT_NO_VIDEO / NO_KEYFRAME / NO_PARAM_SETS / DECODE_FAIL / TOO_SMALL / BLACK_FRAME / BUSY / TIMEOUT`。
- OAM：`snapshot <call-id> [leg] [frames]`、`snapshot_status <job-id>`。
- Web：`POST /api/v1/calls/{id}/snapshot` 启动；`GET /api/v1/calls/snapshot/{job}` 轮询；`GET /api/v1/calls/snapshot/{job}/image` 取 JPEG（路径必须落在 `NSBC_SNAPSHOT_DIR` 内）；前端在途通话页「抓图」按钮直接弹窗预览。

## 6. 代码索引

| 模块 | 文件 |
|---|---|
| codec 注册/参数 | `src/include/media_codec.h`、`src/mod/av/av_codec.c` |
| 视频 RTP | `src/include/rtp_video.h`、`src/media/video/rtp_video.c` |
| 视频转码 | `src/include/video_xcode.h`、`src/media/video/video_xcode.c` |
| 桥接/规划/tap | `src/media/bridge/media_bridge.c` |
| 录像落盘 | `src/media/rec/recorder.c`（`.h264`）、`src/mod/recording/recording.c`（ffmpeg 合成） |
| 抓图 | `src/media/video/snapshot_job.c`、`src/oam/call_commands.c` |
| Web | `web/src/routes/calls.rs`、`web/static/js/pages-live.js`、`web/static/js/app-core.js` |

## 7. 已知不足与风险

- **G1 未编译验证**：本次改动仅做过静态审查与 `cargo check`；C 侧需在构建机执行 `make -j"$(nproc)"` 才能确认。另：`web` 的 `cargo test` 在仓库基线上即编译失败（测试用 `MockCore.canned` 未随 `Arc<Mutex<...>>` 改造更新），与视频改动无关。
- **G2 libx265 依赖**：H.264→H.265 方向需要目标机 FFmpeg 带 `libx265`；缺失时该方向创建转码失败并保持透传。H.265 作为源（解码/录像/抓图前置）不受影响。上线前需 `ffmpeg -encoders | grep 265` 确认。
- **G3 H.265 SDP 参数集缺失**：SDP 只生成 `a=rtpmap`，不生成 `sprop-vps/sps/pps`，完全依赖带内参数集（已开 `repeat-headers=1`）；对端若严格要求 fmtp 可能协商失败或首帧解码失败。
- **G4 抓图仅覆盖 H.264**：VP8/VP9/H.265 腿无法抓图（`snapshot_job` 只解 H.264 AU）。要覆盖需把帧源改挂 `video_xcode` 解包/解码输出。
- **G5 中途开录不生效（非 H.264 腿）**：`rec_xcode` / 复用 sink 仅在 `bridge_confirm` 时按「当时是否有录制会话」建立。通话建立后才开录且无 re-INVITE 时，VP8/VP9/H.265 腿视频不进录像；H.264 腿不受影响。已建立后停录再开录可继续复用。
- **G6 H.265 RTP AP 假设无 DONL**：按 `sprop-max-don-diff=0` 解析 AP（2 字节长度前缀）。带 DONL 的对端会解析错位。
- **G7 H.265 只要求解码器即可作为目标编码**：`xc_video_cid` 对 H.265 只检查 `codec_can_decode`，无 libx265 时 H.264→H.265 方向会走到创建失败（有告警、可回退透传），但不会提前拒绝协商。
- **G8 没有分辨率/码率协商**：转码编码器固定 800kbps/30fps，分辨率随首帧；不解析 SDP `imageattr`/`max-fs`，高分辨率场景画质与带宽不可控。
- **G9 无拥塞控制/自适应**：不处理 RTCP REMB/TWCC/NACK 驱动的码率调整；仅按需发 PLI。
- **G10 抓图抖动**：同腿 2s 内重复抓图会被拒（`BUSY`/间隔限制），并发槽位不足时也会失败；属预期限流。
- **G11 抓图目录一致性**：Web 取图依赖与核心相同的 `NSBC_SNAPSHOT_DIR`；两个 systemd 单元若环境不同，会出现「抓图成功但取图 404」。

## 8. 验证清单（10.0.0.80）

1. 构建：`make -j"$(nproc)"`；`cd web && cargo build`；`ffmpeg -encoders | grep -i 265`。
2. 编码组合（均开录像）：`VP8↔VP8`、`VP8↔H.264`、`VP9↔VP8`、`H.265↔H.265`、`H.265↔H.264`。
3. 预期日志：`video_xcode created src=... dst=...`；复用腿无 `VIDEO_REC_XCODE`；独立录制腿有 `VIDEO_REC_XCODE ... -> H264`；`rec_feed_video` 帧数 > 0。
4. 产物：mp4 可播放、音画同步；抓图 JPEG 可预览。
5. 回归：纯 H.264 通话、纯音频通话、H.264-only 录像不退化。

## 9. 后续路线

- P1：抓图帧源接入 `video_xcode`，覆盖 VP8/VP9/H.265。
- P1：录制链路改为「确认时创建、feed 时动态取会话」，消除 G5。
- P2：H.265 SDP 增加 `sprop-vps/sps/pps`（从首帧参数集缓存回填），并支持 `sprop-max-don-diff`。
- P2：转码分辨率/码率按 SDP 与 RTCP 反馈自适应（G8/G9）。
