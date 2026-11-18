---
title: "AF_PACKET 抓包：虚拟机环境性能优化"
date: 2025-06-01
tags: NIP,性能优化,虚拟机,KVM,virtio,AF_PACKET
summary: "AF_PACKET 抓包在虚拟机/KVM/阿里云环境性能下降根因分析与修复：去 ROLLOVER、绑核改 base+idx% 公式、poll(0) 空转、每线程独立计数、ring 参数 A/B 测试。"
slug: vm-performance
ord: 90
---
AF_PACKET 抓包在虚拟机环境性能下降问题 —— 完整分析记录
目录
问题陈述

根因分析：物理机假设在虚拟化下失效

对各方观点的辨析

逐项改动建议（附完整代码）

验证与监控

改动优先级（严格按顺序）

兜底方案：应用层二次哈希重映射

附录：命令速查与参数速查

一、问题陈述
现象
同一份 AF_PACKET 抓包代码：

物理机部署：表现非常良好，抓包量、吞吐量达标。

虚拟机 / 阿里云 ECS 部署：代码能跑，不崩溃，但抓到的包明显少很多。

初始怀疑
代码中做了"按网卡硬件队列 + CPU 一一对应"的精细 NUMA 亲和绑定，怀疑是虚拟化环境下 NUMA 亲和失效导致性能下降。

业务硬约束
同一条语音流（同一 SSRC / Call-ID / 五元组）必须始终在同一个线程内处理。

这条约束排除了 PACKET_FANOUT_LB（轮询）等破坏流亲和的方案。

二、根因分析：物理机假设在虚拟化下失效
2.1 物理机上的逻辑链条（成立）
text
物理网卡硬件 RSS
   ↓ hash 到硬件 RX 队列
硬件队列中断
   ↓ 绑定到某个 NUMA 节点的 CPU
该 CPU 处理 softirq → AF_PACKET ring
   ↓
用户线程按 NUMA 亲和绑定到同一节点
   ↓ 缓存局部性最优
2.2 虚拟机上的逻辑链条（被替换）
在 KVM / virtio-net / vhost-net 架构下，这条链条从第一环就被替换了：

物理机概念	虚拟机对应物	差异
物理网卡 RX 队列	virtio 软件队列	不是同一回事，由 vhost 投递
硬件 RSS hash	软件计算 hash 或透传	通常缺失或与物理不一致
物理中断 CPU	vhost 线程所在物理核	Guest 内不可见
物理 NUMA 节点	Guest 虚拟 NUMA 或不可见	无因果对应
skb->hash 来自硬件	可能来自内核现算	skb_get_rxhash 可能返回 0
结论：Guest 内基于 NUMA 的精细绑定，绑定的只是 vCPU（一个调度实体），与物理 NUMA 无任何可预测对应关系。

2.3 三个具体失效点
（1）绑核公式硬编码了物理机拓扑
原代码：

c
int bind_cpu = 1 + (if_idx * g_io_threads) + local_io_idx;
if (bind_cpu >= nprocs) bind_cpu = bind_cpu % nprocs;
问题：

Guest 内 nprocs 是 vCPU 数，与物理核心编号无映射。

2 网卡 × 8 线程 = 16 线程，但 Guest 只有 8 vCPU 时，取模后多线程挤到同一 vCPU，表面"每线程绑一核"，实际超订。

阿里云共享型/突发型实例 vCPU 是时间片式的，pin 到哪里都没用，会被 steal。

（2）PACKET_FANOUT_HASH 依赖的 skb hash 在 virtio 下不可靠
物理机：RSS 硬件写 hash 到 skb，直接命中。

virtio-net：除非协商 VIRTIO_NET_F_HASH_REPORT（多数云厂商不默认开），hash 由 Guest 内核 softirq 现算，可能：

退化到 skb_get_rxhash 返回 0 → 全部落第一个 socket；

每个包现算，烧 CPU；

与宿主机物理 RSS 完全无关。

后果：Guest 内"稳定地"把流分到 CPU N，宿主机那侧可能全在物理 NUMA 0 排队。

（3）ROLLOVER 破坏流亲和
c
int fanout_type = PACKET_FANOUT_HASH | PACKET_FANOUT_FLAG_ROLLOVER;
ROLLOVER 的语义：某 socket ring 满时，把包扔给组内其他有空闲的 socket。

在语音场景下：

同一条 RTP 流的包被 ROLLOVER 到另一个线程 → 破坏流亲和，应用层状态机错乱或丢包；

即使应用层容错，跨线程访问同一流上下文 → cache line bouncing，VM 下比物理机贵一个数量级。

关键澄清：ROLLOVER 只在 ring 已满时才触发。如果你的 ring 没满，ROLLOVER 根本没被激活，它不是"包少"的主因。去掉它是正确性修复，不是吞吐修复。

三、对各方观点的辨析
以下是对"另一个 AI"给出的建议逐条辨析，作为决策依据。

✅ 采纳：去掉 PACKET_FANOUT_FLAG_ROLLOVER
正确。保护流亲和，理由充分。

修正表述：不是"包少了的头号嫌疑人"，而是"破坏流亲和的元凶"。包少要另外查。

⚠️ 打折采纳：4MB Block Size
方向对（VM 下 vmexit/vmentry 贵，减少 block 切换有收益），但数字激进。

代价说明：

block 越大 → 内核切换 block 频率越低（收益）；

block 越大 → 队头阻塞粒度越大：单个 block 里一帧卡住，整个 block 回不去内核可用池。

VM 里线程被 CFS 抢走几毫秒是常事，4MB block 更容易让整段 ring 停服。

正确做法：A/B 对比，且保持总 ring 内存不变。

❌ 纠正："IO 线程数必须 ≤ virtio 队列数"
这个因果链是错的。

PACKET_FANOUT_HASH 的分发点在 packet_rcv，在 virtio 队列之后、软中断之上。

virtio 队列数 = 上游并发度（NAPI 上下文数）。

fanout 组内 socket 数 = 下游并发度（用户线程数）。

队列少 = 上游瓶颈，不意味着线程数要少。

真正的硬约束是：IO 线程数 ≤ vCPU 数。

⚠️ 半对半错："vCPU 0 是平等公民，不要跳过"
对的部分：vCPU 0 ≠ 物理 Core 0。

错的部分：Guest 内 ksoftirqd/0、默认 IRQ affinity 仍会往 vCPU 0 聚。

结论：大规格实例跳过 vCPU 0 更合理；小规格（2~4 vCPU）不跳，否则只剩一个核干活。

❌ 不推荐：DMI 检测 + 双分支两套逻辑
DMI 在容器/定制镜像里不可靠；

"是不是 VM"二值判断太粗，阿里云共享型/突发型/增强型最优参数完全不同；

维护成本高，bug 面翻倍。

替代方案：参数化（配置项/环境变量），部署脚本按实测调，二进制保持单一逻辑。

四、逐项改动建议
4.1 【必改】去掉 ROLLOVER
c
// ===== 原代码 =====
// int fanout_type = PACKET_FANOUT_HASH | PACKET_FANOUT_FLAG_ROLLOVER;

// ===== 改为 =====
int fanout_type = PACKET_FANOUT_HASH;   // 纯 HASH，绝不 ROLLOVER

uint16_t if_index_num = (uint16_t)ll.sll_ifindex;
int fanout_arg = (int)if_index_num | (fanout_type << 16);

if (setsockopt(fd, SOL_PACKET, PACKET_FANOUT, &fanout_arg, sizeof(fanout_arg)) < 0) {
    LOG_ERROR("FANOUT failed for %s: %s\n", iface, strerror(errno));
    munmap(*ring_out, ring_len);
    close(fd);
    return -1;
}
理由：ROLLOVER 会把 ring 满后的包扔给组内其他 socket，直接破坏"同 SSRC 同线程"的硬约束。

注意：这是正确性修复，不是吞吐修复。

4.2 【必改】绑核公式改为"避 0 + 均匀取模"
c
// ===== 原代码 =====
// long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
// int bind_cpu = 1 + (if_idx * g_io_threads) + local_io_idx;
// if (bind_cpu >= nprocs) bind_cpu = bind_cpu % nprocs;

// ===== 改为 =====
long nprocs = sysconf(_SC_NPROCESSORS_ONLN);

// 小规格（≤4 vCPU）不跳，大规格跳过 vCPU 0（软中断默认聚在 0）
int base = (nprocs > 4) ? 1 : 0;
int bind_cpu = base + (int)(thread_global_idx % (nprocs - base));

cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(bind_cpu, &cpuset);
pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

char tname[32];
snprintf(tname, sizeof(tname), "io_%s_%d_C%d", current_ifname, local_io_idx, bind_cpu);
pthread_setname_np(pthread_self(), tname);
硬约束：g_io_threads * g_ifname_count 必须 ≤ nprocs，否则就是超订，无论怎么绑都救不了。

4.3 【必改】空转让渡改为非阻塞 poll
c
// ===== 原代码 =====
// if (consecutive_empty > 64) {
//     poll(&pfd, 1, 1);   // 1ms 超时
//     consecutive_empty = 0;
// }

// ===== 改为 =====
if (consecutive_empty > 64) {
    poll(&pfd, 1, 0);   // 非阻塞，纯让出时间片给 CFS
    consecutive_empty = 0;
}
理由：VM 下 vCPU 被 CFS steal 时，poll(timeout=1) 的 1ms 实际挂钟时间可能远大于 1ms，回来时 ring 可能已积压。

4.4 【A/B 对比】Block Size
不要一步换 4MB，分三档跑对照实验，总 ring 内存保持恒定：

c
// 档位 A（原值）：
req.tp_block_size = 256 * 1024;   // 256KB
req.tp_block_nr   = 1024;         // 总 256MB

// 档位 B：
req.tp_block_size = 1024 * 1024;  // 1MB
req.tp_block_nr   = 256;          // 总 256MB

// 档位 C：
req.tp_block_size = 4 * 1024 * 1024;  // 4MB
req.tp_block_nr   = 64;               // 总 256MB

req.tp_frame_size = 2048;
关键：tp_block_nr 必须随 tp_block_size 等比调整，保持总内存不变——否则分不清是 block 大小起的作用还是总容量起的作用。

4.5 【A/B 对比】Frame Size
c
// 可试：
req.tp_frame_size = 2048;   // 原值
// 或
req.tp_frame_size = 4096;   // 按实际 RTP 包大小取整
理由：virtio 不保证 payload 按 cache line 对齐，2048 在 VM 里意义不大。4096 能减少 frame 数量、降低扫环开销。

4.6 【监控改造】每线程独立计数与快照
在 ring_worker_thread 内增加每线程统计：

c
typedef struct {
    uint64_t af_total;
    uint64_t af_udp;
    uint64_t af_rtp;
    uint64_t af_sip;
    uint64_t af_parse_fail;
    uint64_t ring_full_wait_ns;   // 累计 ring 满等待时间
    uint64_t empty_spin;          // 累计空转次数
} thread_stats_t;

// 每线程独立，避免跨核 cache line bouncing
static thread_stats_t __attribute__((aligned(64))) g_thread_stats[MAX_THREADS];

// 在循环中更新本地计数（非原子，线程独享）
g_thread_stats[my_global_id].af_total++;
定期（如每 10s）打印各线程速率，用于判断：

某线程速率 >> 其他 → hash 分布偏斜

所有线程都低 → 上游瓶颈

所有线程空转多 → Guest 收包速率本来就低

五、验证与监控
5.1 改代码前必做的三步（关键）
bash
# 1. 上游是否在丢
ethtool -S eth0 | grep -E 'rx_missed|rx_no_buffer|rx_dropped|rx_fifo'

# 2. virtio 队列数 + 各队列是否均衡
ethtool -l eth0
ethtool -S eth0 | grep rx_queue_ | head -20

# 3. fanout 之后、应用之前的丢包
cat /proc/net/packet
5.2 判读表
现象	结论	修复方向
rx_missed / rx_no_buffer 涨	上游丢，代码救不了	提规格 / 开多队列 / 查宿主 QoS
只有 rx_queue_0_packets 有数	单队列实例	ethtool -L eth0 combined N
各 rx_queue 明显不均	virtio RSS 在 guest 里就没分好	改宿主 hash key（自建 KVM）或接受
/proc/net/packet 某 socket Drops 涨	fanout 之后某线程 ring 满	调 ring 参数
都不涨但业务计数少	parse / BPF 过滤掉了	看 g_af_parse_fail
如果第 1 行在涨，下面所有改动都别做——你在优化一个不是瓶颈的地方。

5.3 验证清单（改完后）
检查项	方法	预期
流亲和未被破坏	应用层日志：同一 SSRC/Call-ID 始终在同一 thread_id	100% 一致
各线程负载均衡	对比各线程 af_total 计数	差异 < 30%（VM 下 hash 不可能完美均匀）
Ring buffer 丢包	/proc/net/packet	相比修改前显著下降
总处理包量	应用层统计	恢复到接近物理机水平
rx_missed / rx_no_buffer	ethtool -S <iface>	不再持续增长
六、改动优先级（严格按顺序）
text
[1] ethtool -S + /proc/net/packet 定位丢在哪一层
       ↓
[2] 如果 rx_missed 涨 → 停，去查规格/队列/QoS
       ↓
[3] 改 fanout_type（去 ROLLOVER）— 保护流亲和
       ↓
[4] 改 bind 公式（base + idx % (nprocs - base)）
       ↓
[5] 改空转让渡（poll timeout=0）
       ↓
[6] 加每线程监控，跑 30s 对比各线程速率
       ↓
[7] 如果线程间偏差 > 30% → guest 内 hash 已偏
       考虑应用层二次哈希重映射（见第七节）
       ↓
[8] A/B block_size（256KB / 1MB / 4MB），保持总内存不变
       ↓
[9] A/B tp_frame_size（2048 / 4096）
七、兜底方案：应用层二次哈希重映射
7.1 触发条件
第 [7] 步发现 guest 内 hash 分布严重不均（某线程拿 60%+），且不能控制宿主 RSS。

7.2 思路
内核 fanout 按 skb hash 把包分到线程 X，线程 X 算 (SSRC 或五元组) 的二次哈希：

若算出属于自己 → 本线程处理；

若不属于自己 → 通过无锁 SPSC 队列转给目标线程。

7.3 代价
转发包多一次内存拷贝 + 跨核唤醒；

只对"错位"部分做，比例可控；

换来线程间真正负载均衡 + 流亲和。

7.4 使用原则
只在第 [7] 步显示严重偏斜时才上，否则别引入这个复杂度。

八、附录：命令速查与参数速查
8.1 命令速查
bash
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
8.2 参数速查
参数	物理机推荐	VM 推荐（实测前）	备注
tp_block_size	256KB	A/B: 256KB / 1MB / 4MB	保持总内存恒定
tp_frame_size	2048	2048 或 4096	A/B
tp_block_nr	按总内存	随 block_size 等比调整	总内存恒定
SO_RCVBUFFORCE	256MB	保留	受限容器可能静默失败
fanout_type	HASH | ROLLOVER	HASH（去 ROLLOVER）	保流亲和
bind_cpu	1 + if_idx*T + local_idx	base + idx % (nprocs - base)	小规格 base=0
空转让渡	poll(..., 1)	poll(..., 0)	VM 下避免长挂钟延迟
8.3 完整改动后的 ring_worker_thread 关键片段
c
static void* ring_worker_thread(void *arg) {
    long thread_global_idx = (long)arg;

    int if_idx = (int)(thread_global_idx / g_io_threads);
    int local_io_idx = (int)(thread_global_idx % g_io_threads);

    if (if_idx >= g_ifname_count) return NULL;
    char *current_ifname = g_ifnames[if_idx];

    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);

    /* ===== 改动 1：绑核公式 ===== */
    int base = (nprocs > 4) ? 1 : 0;
    int bind_cpu = base + (int)(thread_global_idx % (nprocs - base));

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(bind_cpu, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    char tname[32];
    snprintf(tname, sizeof(tname), "io_%s_%d_C%d", current_ifname, local_io_idx, bind_cpu);
    pthread_setname_np(pthread_self(), tname);

    uint8_t *ring_base = NULL;
    int fd = setup_af_packet_worker(current_ifname, &ring_base);
    if (fd < 0) {
        LOG_ERROR("Thread %s_%d: AF_PACKET setup failed\n", current_ifname, local_io_idx);
        exit(1);
    }

    /* ===== 改动 2：每线程本地统计（避免跨核原子操作）===== */
    thread_stats_t *stats = &g_thread_stats[thread_global_idx];

    struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLERR };

    int frame_num = 0;
    int my_global_id = (int)thread_global_idx;
    int consecutive_empty = 0;

    while (!g_stop) {
        struct tpacket2_hdr *hdr =
            (struct tpacket2_hdr *)(ring_base + frame_num * g_tp_frame_size);

        if ((hdr->tp_status & TP_STATUS_USER) == 0) {
            consecutive_empty++;
            if (consecutive_empty > 64) {
                /* ===== 改动 3：非阻塞 poll ===== */
                poll(&pfd, 1, 0);
                consecutive_empty = 0;
            }
            frame_num++;
            if (UNLIKELY(frame_num >= g_tp_frames)) frame_num = 0;
            continue;
        }
        consecutive_empty = 0;

        uint8_t *data = (uint8_t *)hdr + hdr->tp_mac;
        uint32_t caplen = hdr->tp_snaplen;

        int ip_off_hint = -1;
        if (hdr->tp_net > hdr->tp_mac) {
            ip_off_hint = hdr->tp_net - hdr->tp_mac;
        }

        pkt_view_t v;
        stats->af_total++;
        /* 原有全局原子计数可保留用于兼容，也可逐步替换 */

        if (parse_ipv4_udp_packet(data, caplen, &v, ip_off_hint)) {
            stats->af_udp++;
            if (is_rtp_payload(v.payload, v.payload_len)) {
                stats->af_rtp++;
                process_rtp_packet((const ipv4hdr *)v.ip, (const udphdr_t *)v.udp,
                                   v.payload, v.payload_len, my_global_id);
            } else if (is_sip_payload(v.payload, v.payload_len)) {
                stats->af_sip++;
                int64_t wc_ns = (int64_t)hdr->tp_sec * 1000000000LL + (int64_t)hdr->tp_nsec;
                process_sip_packet((const ipv4hdr *)v.ip, (const udphdr_t *)v.udp,
                                   v.payload, v.payload_len, wc_ns);
            }
        } else {
            stats->af_parse_fail++;
        }

        hdr->tp_status = TP_STATUS_KERNEL;
        __atomic_thread_fence(__ATOMIC_RELEASE);

        frame_num++;
        if (UNLIKELY(frame_num >= g_tp_frames)) frame_num = 0;
    }

    if (ring_base != NULL) munmap(ring_base, g_ring_len);
    close(fd);
    return NULL;
}
8.4 完整改动后的 setup_af_packet_worker 关键片段
c
static int setup_af_packet_worker(const char *iface, uint8_t **ring_out) {
    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) return -1;

    int version = TPACKET_V2;
    setsockopt(fd, SOL_PACKET, PACKET_VERSION, &version, sizeof(version));

    int discard = 1;
    setsockopt(fd, SOL_PACKET, PACKET_LOSS, &discard, sizeof(discard));

    int rcvbuf = 256 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof(rcvbuf));

    struct tpacket_req req;
    memset(&req, 0, sizeof(req));

    /* ===== 改动 4：Block Size 参数化（A/B 测试用配置项）===== */
    req.tp_block_size = g_tp_block_size;   // 由配置传入：256KB / 1MB / 4MB
    req.tp_frame_size = g_tp_frame_size;   // 2048 或 4096
    req.tp_block_nr   = g_tp_blocks;       // 随 block_size 等比调整

    req.tp_frame_nr = (req.tp_block_size * req.tp_block_nr) / req.tp_frame_size;
    g_frames_per_block = req.tp_block_size / req.tp_frame_size;
    g_tp_frame_size = req.tp_frame_size;
    g_tp_frames     = req.tp_frame_nr;
    g_ring_len      = (size_t)req.tp_block_size * req.tp_block_nr;

    if (setsockopt(fd, SOL_PACKET, PACKET_RX_RING, &req, sizeof(req)) < 0) {
        close(fd); return -1;
    }

    size_t ring_len = (size_t)req.tp_block_size * req.tp_block_nr;
    *ring_out = mmap(NULL, ring_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (*ring_out == MAP_FAILED) {
        close(fd); return -1;
    }
    madvise(*ring_out, ring_len, MADV_WILLNEED);

    struct sockaddr_ll ll;
    memset(&ll, 0, sizeof(ll));
    ll.sll_family   = AF_PACKET;
    ll.sll_protocol = htons(ETH_P_ALL);

    int is_all = (iface == NULL || strcmp(iface, "all") == 0 || strcmp(iface, "any") == 0);
    if (is_all) {
        ll.sll_ifindex = 0;
    } else {
        ll.sll_ifindex = if_nametoindex(iface);
        if (ll.sll_ifindex == 0) {
            munmap(*ring_out, ring_len);
            close(fd);
            return -1;
        }
    }

    if (bind(fd, (struct sockaddr *)&ll, sizeof(ll)) < 0) {
        munmap(*ring_out, ring_len); close(fd); return -1;
    }

    /* ===== 改动 5：去掉 ROLLOVER，保留 HASH ===== */
    uint16_t if_index_num = (uint16_t)ll.sll_ifindex;
    int fanout_type = PACKET_FANOUT_HASH;   // 原: HASH | ROLLOVER
    int fanout_arg = (int)if_index_num | (fanout_type << 16);

    if (setsockopt(fd, SOL_PACKET, PACKET_FANOUT, &fanout_arg, sizeof(fanout_arg)) < 0) {
        LOG_ERROR("FANOUT failed for %s: %s\n", iface, strerror(errno));
        munmap(*ring_out, ring_len);
        close(fd);
        return -1;
    }

    attach_udp_only_bpf(fd);
    return fd;
}
九、总结
三句话结论
物理机成立的前提（硬件 RSS → 中断 → NUMA → 用户线程）在 virtio + vhost-net 下从第一环就被替换了，Guest 内的精细绑核只是"安慰剂"。

真正的修复优先级：先查上游丢包（ethtool -S），再去 ROLLOVER（保流亲和），再调绑核（避 0 均匀取模），最后 A/B ring 参数。

VM 里做到物理机同等吞吐通常不现实，能做的是把"因为错误假设而损失的 30%"补回来。

禁止清单（明确不要做）
❌ 不要换 PACKET_FANOUT_LB（破坏流亲和）

❌ 不要信"IO 线程数必须 ≤ virtio 队列数"（错误约束）

❌ 不要用 DMI 检测 + 双分支两套逻辑（维护成本高）

❌ 不要一步把 tp_block_size 换 4MB（队头阻塞）

❌ 不要在 rx_missed 增长时改代码（方向错了）

必做清单
✅ 去掉 PACKET_FANOUT_FLAG_ROLLOVER

✅ 绑核改 base + idx % (nprocs - base)

✅ 空转改 poll(timeout=0)

✅ 加每线程独立统计

✅ 改代码前先读 ethtool -S 和 /proc/net/packet

文档版本：v1.0
适用场景：AF_PACKET V2 抓包 + 语音流处理 + KVM/virtio 虚拟化环境


# 抓包运行模式：0=物理机(默认), 1=虚拟机/KVM/腾讯云/阿里云等云主机
# 物理机依赖"硬件RSS→中断→NUMA→用户线程"链条，使用物理拓扑绑核+HASH|ROLLOVER；
# virtio/vhost 云主机下该链条失效，开启后自动切换为：
#   1) FANOUT 去掉 ROLLOVER，保证同一条语音流(SSRC/五元组)始终同一线程处理
#   2) 绑核改为"跳过vCPU0+均匀取模"(≤4 vCPU不跳0)，IO线程总数强制不超过vCPU数
#   3) 空转让渡改为非阻塞 poll(0)，避免 vCPU 被 steal 时 1ms 超时变成长挂钟延迟
#   4) 每线程独立计数，每15秒输出 [VM-IO-STATS] 各线程速率，用于判断hash是否偏斜
# 上线前务必先在虚机内执行（若计数持续增长，属上游规格/队列瓶颈，改本参数无效）：
#   ethtool -S <网卡> | grep -E 'rx_missed|rx_no_buffer|rx_dropped|rx_fifo'
#   cat /proc/net/packet
