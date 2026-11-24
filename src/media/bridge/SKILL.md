# media/bridge 协议红线（RFC 对照）

改动本目录任何 SDP 生成 / 改写 / SIP 头构造代码前，先读本文件。

**优先级**：本文件依据的 RFC 条款 > 现有代码行为。发现代码与本文件冲突时，
按 RFC 修代码，不要为了让现有实现"看起来能跑"而反向放宽规范。
验证一律以**严格实现**为准（Linphone / belle-sip、FreeSWITCH、Kamailio），
MicroSIP 等宽容实现会掩盖非法 SDP。

---

## 0. 四条红线

1. SDP 字段顺序不能乱：媒体级 `c=` 必须在任何 `a=` 之前（RFC 8866 §5）。
2. answer 的 `m=` 行数量、顺序必须与 offer 完全一致；拒绝媒体写 `m=<media> 0 ...`，**不能删段**。
3. answer 的 fmt 只能是 offer 的子集；动态 PT 必须有 `a=rtpmap`，且编码名/时钟率与 offer 一致。
4. 明文腿（`RTP/AVP`）必须剥掉 WebRTC 专有属性与 WebRTC 专用 PT（rtx/red/ulpfec…）。

---

## 1. SDP 语法（RFC 8866 / RFC 4566）

### 1.1 字段顺序

| 层级 | 顺序 |
| --- | --- |
| 会话级 | `v= o= s= i= u= e= p= c= b= t= r= z= k= a=` |
| 媒体级 | `m= i= c= b= k= a=` |

- 会话级 `c=` 必须出现在第一个 `t=` 之前。
- 媒体级 `c=` 必须出现在该段第一个 `a=` 之前。
- 一个媒体段若会话级已有 `c=`，媒体级 `c=` 可省略；但绝不能倒序出现。
- **追加属性（rtpmap/fmtp）写在该段末尾**，不要紧跟 `m=` 行。

### 1.2 `m=` 行

格式：`m=<media> <port> <proto> <fmt> ...`

- `port=0` 表示该媒体流被拒绝（RFC 3264 §6）。
- `proto` 必须与实际承载一致：明文 `RTP/AVP`，WebRTC `UDP/TLS/RTP/SAVPF`。
- fmt 顺序即偏好顺序（RFC 3264 §5.1）。
- fmt 列表必须与 `a=rtpmap` 一一对应：动态 PT 缺 rtpmap、或列表里留着已删除 codec 的 PT，
  都会让严格实现丢弃**整段**媒体。

### 1.3 静态 PT（RFC 3551）

| PT | 编码 | RTP 时钟 |
| --- | --- | --- |
| 0 | PCMU | 8000 |
| 4 | G723 | 8000 |
| 8 | PCMA | 8000 |
| 9 | G722 | **8000**（不是 16000，RFC 3551 §4.5.2） |
| 18 | G729 | 8000 |
| 26 | JPEG | 90000 |
| 31 | H261 | 90000 |
| 34 | H263 | 90000 |

96–127 为动态 PT，必须写 `a=rtpmap`。

### 1.4 方向属性（RFC 3264 §6.1）

`a=sendrecv`（默认）/ `a=sendonly` / `a=recvonly` / `a=inactive`。

- 媒体转发必须按 SDP 方向门控，不能"协商完就双向全通"。
- hold 用 `a=inactive` 或 `c=0.0.0.0`，两者都要能识别。

---

## 2. Offer/Answer（RFC 3264）

- answer 的 `m=` 与 offer 一一对应、顺序相同、数量相同。
- 拒绝：`m=<media> 0 <proto> <fmt>`，属性照抄但端口置 0。
- answer 的 fmt 是 offer 的子集；同一 m= 段的音频/视频只选一组编码（telephone-event 例外）。
- answer 的 `a=rtpmap` 编码名与时钟率必须与 offer 完全一致；fmtp 必须兼容（如 H.264
  `packetization-mode` / `profile-level-id`）。
- re-INVITE 重协商只允许收窄或对等扩展；unsolicited answer 必须校验 SDP 与当前会话一致。

---

## 3. 电话事件（RFC 4733）

- 必须使用动态 PT：`a=rtpmap:126 telephone-event/8000` + `a=fmtp:126 0-16`。
- TE 的 PT 必须与任何音频 codec PT 不同。
- 若对端 answer 未宣告 telephone-event，**禁止**向该腿发 RFC4733 包。
- 收到 TE 包只解析一次（去重后转 IVR），不要重复投递给对端。

---

## 4. WebRTC ↔ 明文 转换

### 4.1 发往明文腿必须剥离

- 属性：`a=ice-ufrag`、`a=ice-pwd`、`a=ice-options`、`a=candidate`、`a=fingerprint`、
  `a=setup`、`a=mid`、`a=group:BUNDLE`、`a=rtcp-mux`、`a=ssrc`、`a=ssrc-group`、
  `a=extmap`、`a=rtcp-fb`、`a=x-google-*`。
- PT：`rtx` / `red` / `ulpfec` / `flexfec` 等 WebRTC 专用编码，必须同时从 `m=` 的 fmt-list
  中删除（`rtx` 在纯 `RTP/AVP` 下没有 `a=fmtp:<pt> apt=<pt>` 就没有意义，RFC 4588）。

### 4.2 发往 WebRTC 腿必须保留/生成

DTLS-SRTP（RFC 5764）、ICE（RFC 8445 / RFC 8839）、rtcp-mux（RFC 5761）、
BUNDLE（RFC 9143）、JSEP（RFC 8829）。SRTP 只在 WebRTC 腿终结，relay 内部走明文。

### 4.3 profile 不得混用

同一 m= 段不能同时宣告 `RTP/AVP` 与 `UDP/TLS/RTP/SAVPF`；两腿各按自己的 profile 生成。

---

## 5. SIP（RFC 3261 及相关）

### 5.1 事务与 ACK

- 2xx 的 ACK 是**新事务**（新 branch），`To` 必须带 UAS 的 tag。
- 非 2xx 的 ACK 复用原 INVITE 的 branch（RFC 3261 §17.1.1.3）。
- 2xx 未被 ACK 时要重传，直到 ACK 或 Timer H（RFC 3261 §13.3.1.4）。

### 5.2 对话

- dialog 标识 = Call-ID + local tag + remote tag；ACK/BYE/re-INVITE 的 tag 必须匹配，
  否则对端回 481。
- `Record-Route` / `Route` 严格按 RFC 3261 §12.2 双向处理。
- `Contact` 必须可路由（含 transport 参数）。

### 5.3 方法语义

CANCEL 只对 1xx 有效（RFC 3261 §9.2）；re-INVITE 改 SDP；UPDATE（RFC 3311）；
PRACK（RFC 3262）；Session Timer（RFC 4028，本仓库对 `Session-Expires` 有上限裁剪）。

### 5.4 SDP 承载

`Content-Type: application/sdp`，`Content-Length` 必须与实际 body 字节数**精确**一致。
长度错会让 belle-sip 截断 body，表现为"对端只答了第一个 codec"。

### 5.5 其他

`Require` / `Supported` 只声明真正实现的特性；`100rel` 仅在 1xx 需要带 SDP 时使用。

---

## 6. 本目录落点

| 行为 | 位置 |
| --- | --- |
| A→B offer 生成/改写 | `bridge_rewrite_sdp()`、`sdp_xc_add_offer_audio()`、`plen_slim_fmt_list()`、`sdp_plain_video_pt_set()` |
| B→A answer 生成/改写 | `sdp_xc_answer_for_a()`、`bridge_rewrite_sdp(..., MB_SDP_ANSWER)` |
| 媒体地址/NAT 兜底 | `offer_hook()` / `answer_hook()` |
| 媒体转发与方向门控 | `relay_recv_one()`（`dir[]`、`pmap[]`、`dtmf_pt[]`） |
| 编码终结与转码计划 | `bridge_confirm()`（`neg_*`、`transcode[]`、`video_xcode[]`） |

---

## 7. 改动前自检清单

1. 新增/删除的 `a=` 行是否落在合法位置（媒体段 `c=` 之后、段内）？
2. `m=` 的 fmt-list 与 `a=rtpmap` 是否一一对应？删 PT 时是否同步删了 rtpmap/fmtp？
3. answer 的 `m=` 数量与顺序是否与 offer 一致？拒绝是否用 `port=0` 而不是删段？
4. 明文腿是否还残留 `ice-*` / `fingerprint` / `setup` / `rtcp-mux` / `ssrc` / `rtx`？
5. 动态 PT 是否都有 rtpmap，编码名与时钟率是否与对端一致？
6. 方向属性是否真正生效，而不是默认双向全通？
7. SDP 字节数与 `Content-Length` 是否精确？
8. 是否用严格实现验证过（优先 Linphone/belle-sip，其次 FreeSWITCH）？

---

## 8. 已知案例（回归参考）

- **2026-11-24　追加 rtpmap 插到 `c=` 之前**：`a=rtpmap:18` / `a=rtpmap:98` 紧跟 `m=` 输出。
  MicroSIP 正常；Linphone 只认 `c=` 之前的属性，于是只答 PT=18（8/126 的 rtpmap 在 `c=`
  之后被忽略），video 段整个从 answer 消失，且因远端地址解析失败而零 RTP 收发。
  修复：`sdp_xc_add_offer_audio()` 用 `pend` 缓冲，在媒体段末尾（下一个 `m=` 或 SDP 结束）flush。
- **会话级 `c=` 写在 `t=` 之后**：Linphone 判 answer 非法 → ACK 后同毫秒 BYE。
- **`m=video` fmt-list 列 21 个 PT 但只有 4 条 rtpmap**：严格实现直接丢弃整段视频。

---

## 9. 参考 RFC

RFC 8866（SDP）/ 4566（SDP 旧版）/ 3264（Offer-Answer）/ 3551（RTP AVP Profile）/
4733（DTMF）/ 3261（SIP）/ 3262（100rel）/ 3311（UPDATE）/ 4028（Session Timer）/
5764（DTLS-SRTP）/ 5761（rtcp-mux）/ 4588（rtx）/ 8445（ICE）/ 8839（ICE in SDP）/
8829（JSEP）/ 9143（BUNDLE）
