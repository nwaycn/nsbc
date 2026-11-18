---
title: 内核抓包在虚拟机环境性能下降根因分析
date: 2025-06-01
tags: NIP,性能优化,虚拟机,KVM,virtio,内核抓包
summary: NIP 内核级抓包在 KVM/virtio/阿里云环境吞吐量下降 30%+ 的根因分析：物理机硬件 RSS→中断→NUMA 链条在虚拟化下被替换，Guest 内精细绑核只是安慰剂；修复优先级：先查上游丢包（ethtool -S），再去掉负载溢出保护、改绑核为均匀取模、空转让渡改为非阻塞。
slug: vm-performance
ord: 90
---

# 内核抓包在虚拟机环境性能下降根因分析

同一份 NIP 内核抓包程序（passive 模式监听 SIP/RTP，无需与 VOIP 系统强耦合）：

- **物理机部署**：抓包量、吞吐量达标。
- **虚拟机 / KVM / 阿里云 ECS / 腾讯云**：程序能跑、不崩溃，但抓到的包明显少 30%+。

本文记录完整的根因分析与修复路线，适用于 NIP、同类内核抓包系统、以及任何对"流亲和"有硬约束的语音流处理程序。

---

## 一、问题陈述

**现象**：同一份抓包代码，物理机表现良好，虚拟机里吞吐打对折甚至更低。

**初始怀疑**：代码中做了"网卡 × CPU 一一对应"的 NUMA 亲和绑定，怀疑是虚拟化环境下 NUMA 亲和失效导致性能下降。

**业务硬约束**：同一条语音流（同一 SSRC / Call-ID / 五元组）必须始终在同一个线程内处理——这条约束排除了轮询分发等破坏流亲和的方案。

---

## 二、根因分析：物理机假设在虚拟化下失效

### 2.1 物理机上的逻辑链条（成立）

```
物理网卡硬件 RSS
   ↓ hash 到硬件 RX 队列
硬件队列中断
   ↓ 绑定到某个 NUMA 节点的 CPU
该 CPU 处理 softirq → 内核抓包 ring
   ↓
用户线程按 NUMA 亲和绑定到同一节点
   ↓ 缓存局部性最优
```

这条链条每一环都有因果关系，所以"精细绑核 = 最优吞吐"在物理机上成立。

### 2.2 虚拟机上的逻辑链条（从第一环就被替换）

在 KVM / virtio-net / vhost-net 架构下：

| 物理机概念 | 虚拟机对应物 | 差异 |
|---|---|---|
| 物理网卡 RX 队列 | virtio 软件队列 | 不是同一回事，由 vhost 线程投递 |
| 硬件 RSS hash | 软件计算 hash 或透传 | 通常缺失或与物理网卡不一致 |
| 物理中断 CPU | vhost 线程所在物理核 | Guest 内不可见、不可控 |
| 物理 NUMA 节点 | Guest 虚拟 NUMA 或不可见 | 与物理 NUMA 无因果对应 |
| skb->hash 来自硬件 RSS | 可能由 Guest 内核现算 | hash 结果不稳定 |

**结论**：Guest 内做的精细绑核，绑定的只是 vCPU（一个被 CFS 调度的时间片实体），与物理 NUMA 拓扑无任何可预测的对应关系。

### 2.3 三个具体失效点

**（1）绑核公式硬编码了物理机拓扑**

物理机上 `bind_cpu = 1 + (if_idx * threads) + local_idx` 能让每个绑到独立核心。但在 Guest 里：

- Guest 报告的是 vCPU 数，不是物理核心数
- 2 网卡 × 8 线程 = 16 线程，但 Guest 只有 8 vCPU 时，取模后多线程挤到同一 vCPU，表面"每线程绑一核"，实际是 2:1 超订
- 阿里云共享型 / 突发型实例的 vCPU 是时间片式的，pin 到哪里都会被 steal

**（2）依赖的包分发 hash 在 virtio 下不可靠**

物理机上 hash 值由 NIC 硬件写入 skb，分发稳定。virtio-net 下：

- 除非协商了 `VIRTIO_NET_F_HASH_REPORT`（多数云厂商不默认开），否则 hash 由 Guest 内核 softirq 现算
- 可能退化到全部包 hash 为 0 → 全部落到第一个处理线程
- 每个包现算 hash 本身也烧 CPU

后果：Guest 内"稳定地"把流分到线程 N，但宿主机上可能全部物理队列堆在同一个 NUMA 节点排队。

**（3）负载溢出保护破坏流亲和**

负载溢出（rollover）的语义：当某个处理线程的 ring 满时，新到的包扔给组内其他有空闲 ring 的线程。

在语音场景下这是致命的：

- 同一条 RTP 流的包被 rollover 到另一个线程 → 破坏流亲和，应用层状态机错乱或丢包
- 即使应用层能容错，跨线程访问同一流上下文 → cache line bouncing，VM 下比物理机贵一个数量级

**关键澄清**：rollover 只在 ring 已满时才触发。如果你的 ring 没满，它根本没被激活。去掉它是**正确性修复**，不是吞吐修复。

---

## 三、各方观点辨析

以下是对常见优化建议逐条辨析，作为决策依据。

| 建议 | 评价 | 理由 |
|---|---|---|
| **去掉 rollover** | ✅ 采纳 | 保护流亲和，理由充分。修正表述：不是"包少了的头号嫌疑人"，而是"破坏流亲和的元凶"。包少要另外查。 |
| **一步把 block size 换 4MB** | ⚠️ 打折采纳 | 方向对（减少内核切换收益），但数字激进。block 越大 → 队头阻塞粒度越大；VM 里 CFS 抢走几毫秒是常事，4MB block 更容易让整段 ring 停服。正确做法：A/B 三档对比，保持总内存恒定。 |
| **IO 线程数必须 ≤ virtio 队列数** | ❌ 纠正 | 因果链是错的。virtio 队列数 = 上游并发度（NAPI 上下文数），fanout 线程数 = 下游并发度（用户线程数），两者独立。真正的硬约束是：**IO 线程数 ≤ vCPU 数**。 |
| **vCPU 0 是平等公民，不要跳过** | ⚠️ 半对半错 | vCPU 0 ≠ 物理 Core 0，但 Guest 内 ksoftirqd/0 和默认 IRQ affinity 仍会往 vCPU 0 聚。大规格（>4 vCPU）跳过 0 更合理；小规格（2~4 vCPU）不跳，否则只剩一个核干活。 |
| **DMI 检测 + 双分支两套逻辑** | ❌ 不推荐 | DMI 在容器 / 定制镜像里不可靠；"是不是 VM"二值判断太粗（共享型 vs 突发型 vs 增强型最优参数完全不同）；维护成本翻倍。替代方案：参数化配置项，部署脚本按实测调，二进制保持单一逻辑。 |

---

## 四、修复路线

### 必改项（正确性修复）

| 顺序 | 改动 | 说明 |
|---|---|---|
| 1 | **去掉负载溢出保护** | 纯 hash 分发，绝不 rollover。同 SSRC 同线程，正确性立竿见影。 |
| 2 | **绑核改为"避 0 + 均匀取模"** | 公式：`bind = base + thread_idx % (nprocs - base)`；小规格 base=0，大规格 base=1（跳过 vCPU 0）。硬约束：IO 线程总数必须 ≤ vCPU 数，否则就是超订，无论怎么绑都救不了。 |
| 3 | **空转让渡改为非阻塞让出** | 连续空轮询 > 64 次后，调用非阻塞 poll(timeout=0) 让出时间片给 CFS。VM 下 vCPU 被 steal 时，poll(1ms) 的 1ms 实际挂钟可能远大于 1ms，回来时 ring 已积压。 |
| 4 | **每线程独立计数** | 每个线程独立维护 af_total / af_udp / af_rtp / af_sip / af_parse_fail，每 10–15 秒打印各线程速率。用于判断：某线程速率 >> 其他 → hash 分布偏斜；所有线程都低 → 上游瓶颈。 |

### A/B 对比项（实测后再定）

- **ring 大小分三档跑对照**（总内存恒定）：256KB block / 1MB block / 4MB block
- **frame size**：2048 或 4096，减少 frame 数量降低扫环开销

---

## 五、验证与监控

### 5.1 改代码前必做三步（关键！）

```bash
# 1. 上游是否在丢
ethtool -S eth0 | grep -E 'rx_missed|rx_no_buffer|rx_dropped|rx_fifo'

# 2. virtio 队列数 + 各队列是否均衡
ethtool -l eth0
ethtool -S eth0 | grep rx_queue_ | head -20

# 3. fanout 之后、应用之前的丢包
cat /proc/net/packet
```

### 5.2 判读表

| 现象 | 结论 | 修复方向 |
|---|---|---|
| rx_missed / rx_no_buffer 持续上涨 | 上游丢，代码救不了 | 提实例规格 / 开多队列 / 查宿主 QoS |
| 只有 rx_queue_0_packets 有数 | 单队列实例 | `ethtool -L eth0 combined N` |
| 各 rx_queue 明显不均 | virtio RSS 在 guest 里就没分好 | 自建 KVM 改宿主 hash key，或接受 |
| /proc/net/packet 某 socket Drops 涨 | fanout 之后某线程 ring 满 | 调 ring 大小 |
| 都不涨但业务计数少 | parse / BPF 过滤掉了 | 看 parse_fail 计数 |

**核心原则**：如果上游丢包在涨，下面所有改动都别做——你在优化一个不是瓶颈的地方。

### 5.3 改完后的验证清单

| 检查项 | 方法 | 预期 |
|---|---|---|
| 流亲和未被破坏 | 应用层日志：同一 SSRC/Call-ID 始终在同一 thread_id | 100% 一致 |
| 各线程负载均衡 | 对比各线程 af_total 计数 | 差异 < 30%（VM 下 hash 不可能完美均匀） |
| Ring buffer 丢包 | /proc/net/packet | 相比修改前显著下降 |
| 总处理包量 | 应用层统计 | 恢复到接近物理机水平 |
| rx_missed | ethtool -S | 不再持续增长 |

---

## 六、修复优先级（严格按顺序）

```
[1] ethtool -S + /proc/net/packet 定位丢在哪一层
       ↓
[2] 如果 rx_missed 涨 → 停，去查规格/队列/QoS
       ↓
[3] 去掉 rollover — 保护流亲和（正确性修复）
       ↓
[4] 绑核改 base + idx % (nprocs - base)
       ↓
[5] 空转让渡改 poll(0)
       ↓
[6] 加每线程监控，跑 30s 对比各线程速率
       ↓
[7] 如果线程间偏差 > 30% → guest 内 hash 已偏
       考虑应用层二次哈希重映射（见第七节）
       ↓
[8] A/B block_size（256KB / 1MB / 4MB），保持总内存不变
       ↓
[9] A/B frame_size（2048 / 4096）
```

---

## 七、兜底方案：应用层二次哈希重映射

### 触发条件

第 [7] 步发现 guest 内 hash 分布严重不均（某线程拿 60%+），且不能控制宿主 RSS。

### 思路

内核 fanout 按 skb hash 把包分到线程 X，线程 X 再算 SSRC 或五元组的二次哈希：

- 若算出属于自己 → 本线程处理
- 若不属于自己 → 通过无锁 SPSC 队列转给目标线程

### 代价

- 转发包多一次内存拷贝 + 跨核唤醒
- 只对"错位"部分做，比例可控
- 换来线程间真正负载均衡 + 流亲和

**使用原则**：只在第 [7] 步显示严重偏斜时才上，否则别引入这个复杂度。

---

## 八、总结

### 三句话结论

1. 物理机成立的前提（硬件 RSS → 中断 → NUMA → 用户线程）在 virtio + vhost-net 下从第一环就被替换了，Guest 内的精细绑核只是"安慰剂"。
2. 真正的修复优先级：**先查上游丢包**（ethtool -S）→ **去掉 rollover**（保流亲和）→ **调绑核**（避 0 均匀取模）→ **最后 A/B ring 参数**。
3. VM 里做到物理机同等吞吐通常不现实，能做的是把"因为错误假设而损失的 30%"补回来。

### 禁止清单（明确不要做）

- ❌ 不要换轮询分发（破坏流亲和）
- ❌ 不要信"IO 线程数必须 ≤ virtio 队列数"（错误约束）
- ❌ 不要用 DMI 检测 + 双分支两套逻辑（维护成本高）
- ❌ 不要一步把 block 换 4MB（队头阻塞）
- ❌ 不要在 rx_missed 增长时改代码（方向错了）

### 必做清单

- ✅ 去掉负载溢出保护
- ✅ 绑核改 base + idx % (nprocs - base)
- ✅ 空转让渡改非阻塞 poll(0)
- ✅ 加每线程独立统计
- ✅ 改代码前先读 ethtool -S 和 /proc/net/packet

---

## 附录：命令速查

```bash
# === Guest 内 ===

# 队列数
ethtool -l eth0

# 各队列收包量
ethtool -S eth0 | grep rx_queue_

# 上游丢包
ethtool -S eth0 | grep -E 'rx_missed|rx_no_buffer|rx_dropped|rx_fifo'

# fanout 之后的丢包
cat /proc/net/packet

# 调整队列数（受实例规格限制）
ethtool -L eth0 combined <N>

# === Host 侧（自建 KVM 才能查）===

# vhost 线程物理分布
ps -eLo pid,tid,psr,comm | grep -E 'vhost|qemu'

# vCPU pinning
virsh vcpupin <vm>
virsh emulatorpin <vm>

# 物理网卡 NUMA 节点
cat /sys/class/net/<phys_if>/device/numa_node
```

---

文档版本：v2.0

适用场景：NIP 内核抓包 + 语音流处理 + KVM/virtio 虚拟化环境 / 阿里云 / 腾讯云等公有云主机
