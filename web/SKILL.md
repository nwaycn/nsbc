# web 模块工程约定

本文件记录 web 模块（Rust 侧）与 C 核心交互时反复踩过的坑，改动相关代码前必读。

---

## MOS / 丢包 / 抖动（在途呼叫质量指标）

### 数据链路

```
bridge (RTP/RTCP 统计)
  └─ bridge_call_view() → mb_view_t (rr_* / rtp_* / gate_open)
       ├─ signaling.c  sig_on_end_cb()  → 呼叫结束时算一次，写回 b2b_call_t
       └─ b2bua.c      call_refresh_vq() → 在途每呼叫 1s 节流刷新
                                            ↓
                              show channels 文本行（key=value）
                                            ↓
                          coreclient.rs  parse_call_list()
                                            ↓
                          pages-live.js  fmtMos() / fmtLoss()
```

### MOS 计算必须用 G.113 / G.107 E-model

统一公式（`src/core/signaling.c` 与 `src/sip/b2bua.c` 两处必须完全一致）：

```c
/* G.711: Ie = 0, Bpl = 25.1 */
float ppl    = loss_permille / 10.0f;        // 千分比 → 百分比
float ie_eff = 95.0f * ppl / (ppl + 25.1f);
float r_val  = 93.2f - ie_eff;               // R ∈ [0, 100]（百分制！）
if (r_val < 0.0f)   r_val = 0.0f;
if (r_val > 100.0f) r_val = 100.0f;

float mos = 1.0f + 0.035f * r_val
          + 7.0f * r_val * (r_val - 60.0f) * (100.0f - r_val) * 1e-6f;
if (mos < 1.0f) mos = 1.0f;
if (mos > 4.5f) mos = 4.5f;

mos_x1000 = (int32_t)(mos * 1000.0f + 0.5f);  // 存 MOS×1000
```

### 禁止的写法（历史上改错过多次）

1. **把千分制 R 直接代入百分制公式**
   ```c
   // 错误：942 是千分制，公式要 0..100
   int32_t r = 942 - loss_q * 25 / 100;
   mos = 0.035f * r + 7.0f * r * (1000 - r) * (r - 640) * 1e-8f;
   // 结果：MOS = 34 → 前端显示 0.03（正常通话被显示成"基本不通话"）
   ```
   **只要看到 `942`、`(1000 - r)`、`(r - 640)`、`1e-8` 这个组合，就是错公式。**

2. **忘记 ×1000**
   `mos_x1000` 字段存的是 MOS×1000。算出 4.41 必须存 4410，
   否则前端 `fmtMos(v) = v/1000` 会显示 `0.00`。

3. **用 `0` 表示"未采集"**
   在途呼叫在第一次 VQ 刷新前 `mos_x1000` 是 memset 的 0，与"真实计算值"无法区分。
   正确做法：
   - C 端：`mos_x1000 <= 0` 时 loss/jitter/mos **统一输出 `-1`**
   - Rust 端：`mos_x1000 == -1` → `None`；`0` 不特殊处理（已被 `> 0` 过滤）
   - 前端：`null` → `-`；`0` 是有效值，丢包显示 `0%`、抖动显示 `0`

4. **在途呼叫不刷新 VQ**
   `sig_on_end_cb` 只在挂断时计算。若删除 `call_refresh_vq()` 的调用，
   在途列表的 MOS/丢包会永远显示 `-`。

### 参考值（G.711，可用于回归验证）

| 丢包 | R | MOS | `mos_x1000` |
|------|---|-----|-------------|
| 0%   | 93.2 | 4.41 | 4410 |
| 1%   | 89.6 | 4.30 | 4300 |
| 5%   | 77.4 | 3.55 | 3550 |
| 10%  | 66.1 | 3.06 | 3060 |

G.711 理论上限是 **4.41**，不会达到 4.5 以上。若数值超过 4500 或低于 1000，
说明公式又被改错了。

### 适用前提

只有 **RELAY 模式**（媒体经过 nsbcd）才有 RTP/RTCP 统计。
BYPASS 模式下 `bridge_call_view` 拿不到 `gate_open`/RTP 计数，三列保持 `-`，
这是数据源缺失，不是 bug。
