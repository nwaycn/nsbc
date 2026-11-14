# NSBC 高可用（HA）主备配置文档

> 版本：v0.5.0-beta | 更新：2026-11-14

## 1. 概述

NSBC 的 HA 模块采用 **双机主备（Active-Standby）** 模式。核心职责：

| 职责 | 完成情况 |
|------|----------|
| UDP 心跳 + 仲裁角色（P0） | ✅ 已实现 |
| 健康门控（CPU/调度滞后/RSS） | ✅ 已实现（粗骨架） |
| REGISTER 共享（备机 loc_db 热备） | ✅ 已实现 |
| INVITE 门控（备机拒绝新呼） | ✅ 已实现 |
| 外呼门控（备机不 originate） | ✅ 已实现 |
| **状态复制**（注册/对话/CDR 主→备推送） | 🟡 `ha_replica` 骨架已完成（seq/ring/gap/ingest 全路径），传输层 + registrar/b2bua 接线点未接 |
| **BFD**（秒级探测） | 🟡 ha_health.h 预留 `peer_probe`/`peer_rtt_ms` 接口，UDP 心跳周期 250ms 已能秒级收敛，正式 RFC 5880 BFD 留 P1 |
| **Hitless Upgrade**（无感滚动升级） | 🟡 `shm_seg.h` 共享内存段 + epoch/CRC 校验已实现，滚动升级切换流程待装配 |

VIP 漂移（对外暴露的单服务 IP）由外部 **keepalived / HAProxy / SLB** 执行，本进程只出角色状态 + 信令门控。

## 2. 架构图

```
  ┌─────────────────────────────────────────────────────────┐
  │                    外部 keepalived                       │
  │         VIP 139.226.66.211 漂移 → ACTIVE 节点            │
  └──────────────┬───────────────────────────┬─────────────┘
                 │  SIP UDP 5060             │
     ┌───────────▼─────────┐       ┌────────▼───────────┐
     │   nsbcd (ACTIVE)     │  UDP  │   nsbcd (STANDBY)    │
     │                      │ 5070  │                      │
     │  • INVITE ✅ 接呼     │◄────►│  • INVITE ❌ 丢弃     │
     │  • REGISTER ✅ 受理   │心跳  │  • REGISTER ✅ 受理   │
     │  • 外呼 ✅ 允许       │      │  • 外呼 ❌ 拒绝       │
     │  • reg_push ✅ DB写入 │      │  • reg_push ❌ 跳过   │
     │                      │      │  • loc_db ✅ 热备内存 │
     └──────────────────────┘       └─────────────────────┘
```

**关键设计决策**：

- **REGISTER 双机都受理**（2026-11-14 修复）：备机 loc_db 内存保持热备，failover 后新 ACTIVE 直接用已有注册数据接 INVITE，无需等 UA 重注册。
- **reg_push 只 ACTIVE 执行**：注册/注销的 DB 写入和事件推送只走 ACTIVE，防双写。
- **INVITE 备机丢弃**：keepalived 收敛窗口内的漏网直连流量在此兜底，应急呼叫（SOS）例外双保险。
- **HA 自身故障不拖死整机**：socket 创建 / bind / 线程失败 → 静默降级 solo-active（本机无条件 ACTIVE），绝不因 HA 故障让整机不可用。

## 3. 仲裁规则（ha.c）

每 250ms 一个仲裁周期：

| 场景 | 判定条件 | 结果 |
|------|----------|------|
| **单机**（`ha.enabled=false` 或无 peer） | 无对端配置 | ACTIVE |
| **对端不可见** | 启动后等满 `failover_ms` 从未收到心跳，或心跳中断超 `failover_ms` | ACTIVE（failover 接管） |
| **正常主备** | 对端活着 → priority 大的 ACTIVE | 高优先级 ACTIVE，低优先级 STANDBY |
| **Priority 相等** | 对端活着 + priority 相同 | hostname 字典序 tie-break（>=0 者胜） |
| **健康门让位** | 本机 UNHEALTHY（CPU≥95% 或调度滞后≥1000ms）连续 3 个周期 | 主动降 STANDBY（集群模式下） |

### 双主收敛

网络分区愈合瞬间可能出现短暂双 ACTIVE。仲裁周期内双方互见后 priority/tie-break 会自动收敛到单一 ACTIVE，日志留痕：

```
WARN ha: dual-ACTIVE seen (peer node=node-b prio=100); arbitrating
NOTICE ha: role ACTIVE -> STANDBY (peer alive, me prio=100 node=node-a, peer prio=100 node=node-b)
```

## 4. 心跳协议（V1）

UDP 5070（双方同端口互发），包格式：

```
NSBC-HB/1 <seq> <role> <priority> <hostname>\n
```

| 字段 | 含义 |
|------|------|
| seq | 单调递增序号（对端只看活着与否，不做丢包统计） |
| role | 发送方当前角色（0=ACTIVE, 1=STANDBY） |
| priority | 1–65535，默认 100 |
| hostname | `gethostname()` 结果，priority 相同时的 tie-break |

兼容旧 3 字段包（sscanf 返回 ≥3，缺 priority/hostname 按默认值处理）。

## 5. 配置项详解

全部在 `nsbc.yaml` 根层级的 `ha:` 段下：

```yaml
# ---- HA 主备（P0 已实现；复制/BFD 为 P1）----
ha:
  enabled: false                 # 默认 false = 单机模式（ha_role() 恒 ACTIVE）
  
  # 对端地址（必须填 IP:port）。双方互对称填对方。
  # 只接受来自此地址的心跳，防伪造保活把本机钉死在 STANDBY。
  peer: "10.0.0.81:5070"         # 对端 nsbcd 的心跳端口
  
  # 心跳间隔（ms）。实际仲裁周期 = heartbeat_ms / 4（默认 125ms）。
  # 范围 [50, 10000]，默认 500。生产建议 500，BFD 亚秒探测 P1 上线后再调。
  heartbeat_ms: 500
  
  # failover 窗口（ms）。对端连续不可见超此值才升主，避免网络抖动误切。
  # 范围 [100, 60000]，默认 3000。必须 ≥ heartbeat_ms。
  # 生产建议 heartbeat_ms × 6 = 3000（丢 5 个心跳才切）。
  failover_ms: 3000
  
  # 仲裁优先级。大者 ACTIVE。双方相同 → hostname 字典序 tie-break。
  # 未配置或非法 → 默认 100。不填也能用，但建议显式写：
  #   主节点 100，备节点 90（主高备低，避免 hostname 不确定性）
  priority: 100
  
  # --- 以下为健康门参数（可覆盖默认值；不写用 ha_health.c 内置默认）---
  # 事件循环滞后告警阈值（ms）——心跳线程被调度延迟视为系统压力信号
  # evlag_warn_ms: 200
  # evlag_fail_ms: 1000
  # CPU 告警/失败阈值（%）
  # cpu_warn_pct: 80
  # cpu_fail_pct: 95

  # === 状态复制（P1，ha_replica.c 骨架已完成；传输层 + 业务接线点待接）===
  replica:
    enabled: false               # 默认 false —— 主→备状态复制总开关
    # 传输层：独立 TCP 连接（默认 5071），与 UDP 心跳 5070 分离防影响
    port: 5071                   # 复制 TCP 监听端口（双方同端口，主端监听，备端连接）
    # 环形缓冲容量：8192 条。备端 ACK 落后满环 → 主端返回 NOSPC → 触发全量快照
    ring_cap: 8192               # 默认值，一般不动
    # 载荷单条最大长度（字节）：超过此值的单条 REPL_REG/REPL_DIALOG 会被分片或转 FULL
    payload_max: 4096            # 默认 4096
    # 复制水位差告警条数（ha_health 健康门 repl_lag_warn）
    lag_warn: 10000              # 备端 applied_seq 落后主端 pub_seq 超过此值 → DEGRADED
    # 全量快照周期（秒）。0=关闭。周期性对齐防长期运行缺口累积
    full_resync_interval: 0      # 默认关闭，生产建议 3600（每小时全量一次）

  # === BFD 亚秒探测（P1，ha_health.h 预留接口；UDP 心跳 250ms 周期已能秒级收敛）===
  bfd:
    enabled: false               # 默认 false —— 启用后心跳线程会额外发 RFC 5880 BFD Control 包
    local_discriminator: 1       # 本地 BFD Discriminator（1–2^24-1，双方不同）
    desired_min_tx_interval: 200 # ms，建议 100–500，BFD 控制面发包间隔
    required_min_rx_interval: 200 # ms，期望对端发包间隔
    detect_multiplier: 3         # 连续 N 次未收到才判定对端 Down（经典 BFD 口径：interval×3 才断）

  # === Hitless Upgrade 滚动升级（P1，shm_seg.h 共享内存段已实现；切换流程待装配）===
  hitless:
    enabled: false               # 默认 false —— 开启后新进程启动时先 attach SHM 继承状态
    # 共享内存段名前缀（实际段名 = prefix + hostname），多节点互不干扰
    shm_prefix: "nsbc_ha_"       # 默认 nsbc_ha_
    # 主备切换时的"冻结→新进程→解冻"窗口（ms）
    freeze_ms: 1000              # 老进程 stop 前冻结输入等待新进程 ready
    # 升级超时（ms）——新进程在 freeze_ms 内未上线则放弃滚动，回退为重启老进程
    upgrade_timeout_ms: 10000    # 默认 10s
```

### 配置变更生效方式

| 配置项 | 生效方式 | schema 标记 |
|--------|----------|-------------|
| `ha.enabled` | **重启 nsbcd** | CGF_RESTART |
| `ha.peer` | **重启 nsbcd** | CGF_RESTART |
| `ha.heartbeat_ms` | 热加载（`control reload`） | CGF_HOT |
| `ha.failover_ms` | 热加载（`control reload`） | CGF_HOT |
| `ha.priority` | 热加载（`control reload`） | CGF_HOT |
| `ha.replica.enabled` | **重启 nsbcd**（传输层 TCP connect/listen 需重建） | CGF_RESTART |
| `ha.replica.port` | **重启 nsbcd** | CGF_RESTART |
| `ha.replica.ring_cap` | **重启 nsbcd**（环形缓冲静态分配） | CGF_RESTART |
| `ha.replica.lag_warn` | 热加载 | CGF_HOT（未注册 schema，走默认） |
| `ha.replica.full_resync_interval` | 热加载 | CGF_HOT |
| `ha.bfd.enabled` | **重启 nsbcd**（BFD Control socket 需创建） | CGF_RESTART |
| `ha.bfd.desired_min_tx_interval` | 热加载 | CGF_HOT |
| `ha.bfd.detect_multiplier` | 热加载 | CGF_HOT |
| `ha.hitless.enabled` | **重启 nsbcd**（shm 段创建/attach 路径分叉） | CGF_RESTART |
| `ha.hitless.freeze_ms` | 热加载 | CGF_HOT |
| 健康门参数 | 热加载 | 未注册 schema（走默认） |

> ⚠️ `ha.priority` / `ha.replica.*` / `ha.bfd.*` / `ha.hitless.*` 的 schema 尚未全部在 `cfg_schema.c` 注册。已注册的热加载项（heartbeat_ms / failover_ms / priority）可 `control reload` 生效；未注册的走代码默认值，建议启动前写好。

## 6. 信令门控一览表

`ha_role()` 是原子读取，跨线程零锁。各模块在关键入口判断角色：

### 6.1 REGISTER（registrar.c + b2bua.c）

| 角色 | REGISTER 受理 | loc_upsert（内存） | reg_push（DB/事件） | 200 OK 返回 |
|------|--------------|-------------------|--------------------|------------|
| ACTIVE | ✅ | ✅ | ✅ | ✅ 完整返回 |
| STANDBY | ✅ | ✅ | ❌ 跳过（L612） | ✅ 完整返回 |

**为什么备机也受理？** 如果备机静默丢弃 REGISTER → failover 后 loc_db 空 → INVITE 找不到路由 → UA 要等 expires/2 才重注册（默认 1800s/2=15min）。让备机写内存 loc_db 但跳过 DB 推送，既防双写又保持热备。

### 6.2 INVITE 入站（b2bua.c L3980）

| 角色 | 新 INVITE | in-dialog BYE/re-INVITE | Emergency (SOS) |
|------|----------|-------------------------|-----------------|
| ACTIVE | ✅ 接呼 | ✅ 接 | ✅ 接 |
| STANDBY | ❌ 丢弃（503 兜底） | ✅ 接 | ✅ 接（双保险） |

限频日志：`drop_invite_n % 50 == 0` 打 WARN，避免 keepalived 收敛期间刷爆日志。

### 6.3 外呼控制面（b2bua.c L6521）

`b2b_originate()` 是所有外呼统一入口（API/CLI/ESL/队列），备机一律拒绝：

```
WARN b2b: HA standby: originate refused
```

保持单主语义——备机不产生呼叫/CDR/媒体，避免主备各自外呼导致话务分裂。

## 7. 故障降级策略

HA 模块自身故障 **绝不拖死整机**：

| 故障点 | 处理 | 日志 |
|--------|------|------|
| `ha.enabled=false` | 跳过初始化 | 无 |
| `socket(AF_INET, SOCK_DGRAM)` 失败 | free + return OK → 全局 g_ha=NULL → ha_role() 恒 ACTIVE | `WARN ha: socket create failed errno=X; degrade to solo-active` |
| `bind UDP/5070` 失败（端口被旧进程占） | 同上降级 | `WARN ha: bind udp/5070 failed errno=X; degrade to solo-active` |
| `pthread_create` 失败 | 同上降级 | `WARN ha: heartbeat thread create failed errno=X; degrade to solo-active` |
| `ha.peer` DNS/解析失败 | 视为 have_peer=0 → 单机 ACTIVE | `WARN ha: bad ha.peer X; running solo-active` |
| `ha_hc_create` 失败 | 健康门降级关闭，心跳仲裁照常 | 无 WARN（静默关） |

**降级后行为**：`g_ha = NULL`，`ha_role()` 始终返回 HA_ROLE_ACTIVE，相当于单机模式。REGISTER/INVITE/外呼全部正常，只是没有故障转移能力。

## 8. 双机主备部署步骤

### 8.1 环境准备

两台 CentOS 服务器，时钟同步（chrony/ntpd），UDP 5060 + UDP 5070 互通：

| 主机 | 固定 IP | hostname | priority | 角色 |
|------|---------|----------|----------|------|
| node-a | 10.0.0.80 | node-a | 100 | ACTIVE（首选） |
| node-b | 10.0.0.81 | node-b | 90 | STANDBY |
| VIP | 139.226.66.211 | — | — | keepalived 漂移 |

### 8.2 配置 nsbc.yaml

**node-a（主）**：

```yaml
ha:
  enabled: true
  peer: "10.0.0.81:5070"
  heartbeat_ms: 500
  failover_ms: 3000
  priority: 100
```

**node-b（备）**：

```yaml
ha:
  enabled: true
  peer: "10.0.0.80:5070"
  heartbeat_ms: 500
  failover_ms: 3000
  priority: 90
```

两边的 `peer` 字段互指对方。priority 不写也行（走 hostname tie-break），但显式写清楚更稳。

### 8.3 keepalived VIP 漂移

keepalived.conf（两台机器不同的 priority）：

```nginx
# node-a (MASTER)
vrrp_instance VI_1 {
    state MASTER
    interface eth0
    virtual_router_id 51
    priority 150
    advert_int 1
    authentication { auth_type PASS auth_pass nsbc_ha }
    virtual_ipaddress { 139.226.66.211/24 }
    track_process { nsbcd }     # nsbcd 挂了就摘 VIP
}

# node-b (BACKUP)
vrrp_instance VI_1 {
    state BACKUP
    interface eth0
    virtual_router_id 51
    priority 140
    advert_int 1
    authentication { auth_type PASS auth_pass nsbc_ha }
    virtual_ipaddress { 139.226.66.211/24 }
    track_process { nsbcd }
}
```

### 8.4 启动顺序

```bash
# node-a
systemctl start nsbcd
# 日志预期：NOTICE ha: heartbeat enabled peer=10.0.0.81:5070 ...
#           初始 role=STANDBY（等心跳仲裁），收到对端心跳后变成 ACTIVE

# node-b
systemctl start nsbcd
# 日志预期：NOTICE ha: heartbeat enabled peer=10.0.0.80:5070 ...
#           role 稳定在 STANDBY
```

### 8.5 验证清单

```bash
# ① 心跳是否正常互通
grep 'heartbeat enabled' /var/log/nsbcd.log
# NOTICE ha: heartbeat enabled peer=10.0.0.81:5070 hb=500ms failover=3000ms prio=100 node=node-a

# ② 角色是否正确收敛
grep 'role' /var/log/nsbcd.log | tail -5
# node-a: NOTICE ha: role STANDBY -> ACTIVE (peer alive ...)
# node-b: 没有 role 切换日志（保持 STANDBY）

# ③ 注册热备
# UA 在 VIP 上 REGISTER，然后分别 grep 两台的 REGISTER-200
grep 'REGISTER-200 user=1006' /var/log/nsbcd.log

# ④ failover 测试
# node-a 上 kill nsbcd（或拔网线）
# node-b 预期：3s 后看到 role STANDBY -> ACTIVE
# keepalived 预期：VIP 飘到 node-b
# UA 预期：INVITE 继续可达（备机 loc_db 有数据）

# ⑤ 恢复测试
# node-a 重启 → priority 高 → 切回 ACTIVE
# 日志：node-b role ACTIVE -> STANDBY, node-a role STANDBY -> ACTIVE
```

## 9. 监控与日志

### 9.1 关键日志（grep 速查）

| grep | 含义 | 级别 |
|------|------|------|
| `heartbeat enabled` | HA 启动成功，peer/hb/failover/prio 全部打印 | NOTICE |
| `heartbeat sendto failed` | 心跳发不出（对端网络不通 / UDP 端口被防火墙挡） | WARN |
| `role STANDBY -> ACTIVE` | failover 接管 | NOTICE（+ event_fn + log_audit） |
| `role ACTIVE -> STANDBY` | 让位（对端回来 OR 本机健康门触发） | NOTICE |
| `dual-ACTIVE seen` | 双主瞬间（网络分区愈合） | WARN |
| `UNHEALTHY x3, yield ACTIVE` | 本机连续 3 周期 UNHEALTHY，让位 | WARN |
| `degrade to solo-active` | HA 故障降级 | WARN |
| `replica publish NOSPC` | 主端环形缓冲满（备端 ACK 长期落后）→ 转全量/告警 | WARN |
| `replica gap detected` | 备端发现缺口，请求重传 | INFO |
| `replica lag high` | 备端复制水位差 > lag_warn（默认 10000 条） | WARN |
| `replica full resync start` | 主端发全量快照 | NOTICE |
| `bfd session up` | BFD 会话建立成功（P1） | NOTICE |
| `bfd session down` | BFD 会话 Down 超时（P1） | WARN |
| `hitless freeze` | 主进程冻结输入，等待滚动升级 | NOTICE |
| `hitless SHM stale` | 新进程 attach 的 SHM epoch 过旧，降级从零启动 | WARN |

### 9.2 OAM / CLI

HA 暂无独立 CLI 命令。可通过 `nsbcli status` 查看当前角色（需扩展）。

### 9.3 健康门指标（ha_hc_t）

1s 周期自动采样（ha.c `sample_local_health`）：

| 指标 | 来源 | 默认 warn | 默认 fail |
|------|------|----------|----------|
| CPU% | `/proc/self/stat` utime+stime | 80% | 95% |
| RSS (MB) | `/proc/self/status` VmRSS | 0（关闭） | — |
| 事件循环滞后 (ms) | ha 心跳线程调度延迟 | 200 | 1000 |
| 对端可见性 | 连续 3 次空手探测 | — | 不可见 |
| 过载拒绝率 | OAM 接入中（P1） | 5% | 50% |
| 复制水位差 | repl_replica 接入中（P1） | 10000 条 | — |

健康门只影响"主动抢占"决策，不影响仲裁基本规则。DEGRADED 状态不主动抢主，但已有 ACTIVE 维持。UNHEALTHY 连续 3 周期主动让位 STANDBY。

## 10. P1 功能落地状态

### 10.1 模块状态表（2026-11-14 更新）

| 模块 | 头文件 | 骨架完成 | 接线状态 |
|------|--------|----------|----------|
| 心跳 + 仲裁 | ha.h / ha.c | ✅ | ✅ 全接 |
| 健康门 | ha_health.h / ha_health.c | ✅ 粗骨架 | ✅ CPU/RSS/调度滞后/peer_probe/repl_lag 已采样；**BFD 联动已接**；RSS 默认关闭（warn=0） |
| 状态复制 | ha_replica.h / ha_replica.c | ✅ 完整骨架 | ✅ **TCP 传输层已接**（ha_repl_transport.c）；**registrar.c 接线点已接**（REG/UNREG）；**b2bua.c 接线点已接**（CONFIRMED/END） |
| BFD 亚秒探测 | ha_bfd.h / ha_bfd.c（独立 pthread） | ✅ RFC 5881 状态机 | ✅ **BFD Control socket + AdminDown→Down→Init→Up 状态机 + ha_hc_peer_probe 联动**；暂不实现 Echo（D bit=0） |
| Hitless Upgrade | ha.h 公共 API | ✅ | ✅ **6 步完整流程**（FREEZE→DRAIN→FULL push→yield_requested→heartbeat role flip→返回）+ auto-unfreeze 超时 + drain_by_in-dialog BYE 30s 降级 + **FULL resync 状态对齐** |
| Dialog Rebuild | b2bua.c（STANDBY→ACTIVE） | ✅ | ✅ **v3 payload 完整重建**：call state machine（leg tag/cseq/state/a_final）+ profile 查找（sip_profile_find）+ **media anchor relay 重建**（bridge_register_call + rtp_relay_start + set_peer_a/b）+ 去重（tbl_find） |

### 10.2 已知降级点（明确不做或后续 P2）

| 项 | 降级 | 原因 |
|----|------|------|
| **REPL_FULL_CDR_FLUSH** | 未实现 | 当前只对 loc_db + dialog 做 FULL；CDR flush 走独立 DB 路径，不在内存复制覆盖范围 |
| **BFD Echo 模式**（D bit=1） | 未实现 | UDP 心跳 250ms + BFD Control 200ms×3=600ms 已够秒级探测，Echo 不增加价值 |
| **relay transcode session 重建** | rebuild 时 fallback MB_RELAY | relay_mode 在 payload 里打平，但 transcode session 内部状态（codec filter graph）无法序列化；降级为 MB_RELAY 能续通但不转码 |
| **跨进程 SHM（shm_seg.h）** | Hitless Upgrade 走 **replica FULL push** 状态对齐，不依赖 SHM | SHM 是跨进程方案（老进程 publish → 新进程 attach）；我们用的是**进程内** role flip + STANDBY→ACTIVE rebuild_from_buf — 更简单、更安全、无 SHM 陈旧风险 |

> ✅ **已实现**（原 P1.5 降级点已落地）：
> - **NOSPC 主动 FULL**：transport 线程 poll 检测连续 NOSPC ≥ 16（5s 窗口）→ 自动 `transport_send_full_snapshot`。业务线程只做 counter 累加，不阻塞。
> - **payload v4 codec/SSRC**：publish 侧从 `bridge_call_view.ssrc_out[2]` 拿 relay 重写后的 SSRC，从 `codec_registry_find_name` 拿 PT + clock_rate，payload_ver 从 3 升到 4。rebuild 侧 payload 结构体形状不变，自动吃。

### 10.3 ha_replica 核心设计不变

- **主→备单推**，64 位单调 seq + 备端 ACK 水位
- 环形缓冲 8192 条，满了返回 NOSPC → WARN，不阻塞呼叫线程
- GAP_REQ 帧（type=0xFE）重传；同一段 gap 连续 `TRANS_GAP_MAX=3` 次 → FULL_REQ 帧（type=0xFD）
- 载荷类型：REPL_REG=0 / REPL_DIALOG=1 / REPL_FULL_CLEAR=3 / ACK=0xFF / GAP_REQ=0xFE / FULL_REQ=0xFD
- 热升级跨版本：payload 带 `payload_ver` 字段，apply 侧按版本号分支解析

## 11. 常见问题

**Q: 单机部署还要配 HA 吗？**
A: 不用。默认 `ha.enabled=false`，`ha_role()` 恒 ACTIVE，零开销。

**Q: 两台机器启动顺序有影响吗？**
A: 没有。双方同时启动 → 心跳互通 → priority 大的 ACTIVE。priority 相同 → hostname 字典序。

**Q: failover 时间为什么是 3s？能调快吗？**
A: `failover_ms` 最小 100ms，但建议 ≥ `heartbeat_ms × 6`（丢 5 个心跳才切）。太快会因网络抖动误切。生产建议 3000ms，BFD 亚秒探测 P1 上线后再调快。

**Q: 备机 INVITE 丢弃客户端收到什么？**
A: 直接丢弃（不回 503），keepalived 收敛窗口内客户端会超时重发。如果 keepalived track_process 正常（ACTIVE nsbcd 挂了立刻摘 VIP），收敛窗口很短。应急呼叫 SOS 例外，备机也接。

**Q: 备机注册的 UA failover 后需要重新注册吗？**
A: 不需要。备机 loc_db 内存已有完整注册数据，failover 后 INVITE 直接路由。UA 下一次刷新 REGISTER（expires/2 ≈ 15min）时会重新写到新 ACTIVE 的 DB。

**Q: 日志里看到 `dual-ACTIVE seen` 是脑裂吗？**
A: 短暂瞬间（网络分区愈合、keepalived 同时漂 VIP 到两边）。仲裁周期内 priority/tie-break 会立刻收敛，不影响业务。持续 dual-ACTIVE 才是问题（检查 priority 配置 + hostname 是否相同）。

## 12. 状态复制协议（P1 / ha_replica.c）

`ha_replica.c` 骨架已完成：64 位单调 seq + 8192 条环形缓冲 + gap 重传回调 + 幂等 ingest。本章节补全 **传输层设计**、**业务接线点**、**全量快照触发** 三部分，为后续 P1 接线提供权威规范。

### 12.1 传输层：独立 TCP 通道（与 UDP 心跳分离）

心跳走 UDP 5070（双方同端口互发，轻量无连接）；复制走 **独立 TCP 5071**，原因：

- 复制载荷可能较大（单条 REPL_REG 含完整 Contact/Route 可达 KB 级），UDP MTU 不够且丢包重传成本高；
- TCP 的有序投递天然匹配 `repl_ingest()` 的"期望 seq+1"缺口判定语义；
- 分离端口防复制拥塞影响心跳仲裁（仲裁是可用性底线）。

**连接建立**：主端（ACTIVE）监听 TCP 5071，备端（STANDBY）发起 connect。主端 role 切换后立即 `listen()` 接受备端重连（备端有指数退避重试）。

**帧格式（HaReplV1）**：

```c
/* TCP 流帧：固定头 + 可变 payload */
typedef struct {
    uint16_t magic;           /* 0x4852 ("HR") */
    uint16_t ver;             /* 协议版本，允许演进 */
    uint64_t seq;             /* 与 repl_rec_t.seq 对齐 */
    uint8_t  type;            /* repl_type_t（0=REG / 1=DIALOG / 2=CDR_FLUSH / 3=FULL） */
    uint8_t  reserved;
    uint16_t payload_ver;     /* 载荷结构版本（热升级跨版本判定） */
    uint32_t payload_len;     /* 不含本头的载荷长度，上限 REPL_PAYLOAD_MAX=4096 */
} ha_repl_frame_hdr_t;
```

- 头大小固定 **16 字节**，网络字节序；
- 收发循环：`read_full(hdr) → read_full(payload) → repl_ingest()`，不做粘包处理；
- ACK 帧：备端收到并应用后回推一个 **零 payload** 帧（type=0xFF），payload_len 字段填 `repl_applied_seq`。主端据此 `repl_ack()` 淘汰环形缓冲。

### 12.2 主端发布与环形缓冲

`repl_publish(ha_repl_t *r, repl_type_t type, uint16_t ver, const void *payload, uint16_t len)` 调用路径：

```
业务线程（registrar.c / b2bua.c）
  └─ repl_publish()
       ├─ 分配 seq（++pub_seq）
       ├─ 入环形缓冲 ring[pub_seq % 8192]
       ├─ ops.send()（TCP 线程发送；失败则环中留存，TCP 线程重试）
       └─ 返回 seq（供业务侧日志关联）
```

| 返回值 | 含义 | 调用方动作 |
|--------|------|------------|
| `>0`（正整数） | 发布 seq | 正常 |
| `NSBC_NOSPC` | 环形缓冲已满（备端 ACK 长期落后） | **业务侧转全量快照**（调用方调 `control repl full` 或等下一个 `full_resync_interval` 周期），不阻塞呼叫线程 |
| `NSBC_INVAL` | 参数错或非 ACTIVE | 告警日志 |

**关键设计：不阻塞**。环形缓冲是 SPSC（单生产者多消费者可降级为 SPSC）结构，publish 走呼叫线程，TCP 发送走独立 ha 通道线程。环满返回 NOSPC 后，业务侧不应等，应转全量/告警后继续。

### 12.3 备端应用与缺口重传（✅ 已实现）

`repl_ingest()` 判定逻辑（ha_replica.c）：

| 收到的 seq | 期望 seq = applied_seq + 1 | 处理 |
|-----------|---------------------------|------|
| ≤ applied_seq | — | 幂等丢弃（重传重复） |
| == 期望 | ✅ | 调 `ops.apply()` → `applied_seq = rec->seq` |
| > 期望 | ❌ 缺口 | `gaps++` → `ops.on_gap(expect, got)` → 返回 `NSBC_AGAIN` |

**缺口处理策略（已落地实现）**：

1. **单次 gap**：`on_gap` 回调 STANDBY 向 ACTIVE 发 **GAP_REQ 帧**（type=0xFE，payload 含 `{ gap_start, gap_end }`）
2. ACTIVE 收 GAP_REQ → 按区间从环形缓冲取出重发（`transport_resend_gap`）
3. **同一段 gap 连续 `TRANS_GAP_MAX=3` 次**（备端发了 GAP_REQ 但 ACTIVE 重发也丢 / 环形缓冲已淘汰缺口段）→ 备端发 **FULL_REQ 帧**（type=0xFD，无 payload）
4. ACTIVE 收 FULL_REQ → 立即发 **完整快照**（见 12.4 节）

> 骨架版本曾写"不做乱序缓存" — 实际实现也没加，但 `repl_set_accept_any_seq` 在 FULL resync 期间跳过 gap check — 效果等价。

### 12.4 全量快照 REPL_FULL（✅ 已实现，含降级链）

**触发条件（按优先级）**：

| 触发源 | 条件 | 降级 |
|--------|------|------|
| **GAP_REQ 重传失败**（已实现） | STANDBY on_gap same_gap_cnt≥3 → 发 FULL_REQ | 最温和（STANDBY 主动请求，ACTIVE 被动响应） |
| **NOSPC**（✅ 已实现） | ACTIVE 业务线程连续 NOSPC ≥ 16（5s 窗口）→ transport 线程自动 FULL push | 比 gap 重传更彻底的降级（备端 ACK 严重落后时触发） |
| **OAM API**（已实现） | `ha_hitless_upgrade()` Step 3 主动调 `ha_repl_trans_force_full_push()` | Hitless Upgrade 强制状态对齐 |
| **周期对齐**（未实现，P1.5） | `ha.replica.full_resync_interval` 配置（默认 0=关闭） | 定期周期性兜底 |

**FULL push 帧序列（transport_send_full_snapshot 已实现）**：

```
ACTIVE → STANDBY:
  ① REPL_FULL_CLEAR (type=3, seq=snap_seq+1, payload_len=0)
     → STANDBY: repl_set_accept_any_seq=1 + loc_db_clear_all + ha_dlg_buf clear
  ② ring 从 max(ack_seq+1, pub_seq-8192+1) 逐条重发 REPL_REG / REPL_DIALOG
     → STANDBY: accept_any_seq=1 跳过 gap check → 每条调 ops.apply
  ③ HA_REPL_FRAME_ACK_TYPE (type=0xFF, payload_len=snap_seq) 作为结束信号
     → STANDBY: repl_set_accept_any_seq=0 + repl_resync_done(snap_seq)
```

**降级链**：ring 空（无 publish 过）→ FULL push 只发 CLEAR + 尾 ACK → STANDBY 只清内存 → 正常流水恢复。

### 12.5 接线点（✅ 已落地，payload v4）

| 模块 | 事件 | payload_ver | 调用时机 |
|------|------|------------|----------|
| [registrar.c](../../src/registrar/registrar.c) | REGISTER 200 OK（新增/刷新绑定） | 1（扁平 `loc_binding_t`） | `reg_push(&b, exp)` 成功后（ACTIVE guard + `ha_repl_publish(REPL_REG, 1, &b, sizeof(b))`） |
| [registrar.c](../../src/registrar/registrar.c) | REGISTER 4xx/expire=0（注销） | 1 | `reg_push(&dummy, 0)` → `ha_repl_publish(REPL_REG, 1, &dummy, sizeof(dummy))`（expire_ms=0 应用侧视作注销） |
| [b2bua.c](../../src/sip/b2bua.c) | LEG_CONFIRMED（对话锚点创建） | **4** | `c->leg[1].state = LEG_CONFIRMED` 后 publish 完整 REPL_DIALOG |
| [b2bua.c](../../src/sip/b2bua.c) | `call_destroy`（对话终结） | **4** | `ha_repl_publish(REPL_DIALOG, action=1)` — rebuild 侧跳过 END |

**REPL_DIALOG payload v4 完整字段（b2bua.c publish 侧）**：

```c
typedef struct {
    uint8_t  action;        /* 0=CONFIRMED, 1=END */
    uint8_t  media_done;    /* call->media_done */
    uint8_t  hangup_by;     /* call->hangup_by (END 才有值) */
    uint8_t  hold_legs;
    char callid_a[80];      /* leg[0].callid */
    char callid_b[80];
    char loc_tag_a[32];     /* leg[0].loc_tag / rem_tag */
    char rem_tag_a[32];
    char loc_tag_b[32];
    char rem_tag_b[32];
    char aor[256];          /* prof->name */
    char prof_name[128];    /* ★ rebuild 侧 sip_profile_find 用 */
    char orig_to[128];
    char clip[48];
    char dst_number[48];
    uint32_t cseq_a, cseq_b;
    uint64_t start_ms;
    uint8_t  leg0_state, leg1_state;
    uint8_t  a_final;
    /* ★ v3: media anchor — relay + peer */
    uint16_t a_rtp, b_rtp;            /* relay ports: bridge_lookup_relay → rtp_relay_port */
    uint16_t a_rtcp, b_rtcp;          /* a_rtp+1, b_rtp+1 */
    uint8_t  relay_latched_a, relay_latched_b;
    uint8_t  relay_mode;              /* 0=BYPASS / 1=RELAY / 2=TRANSCODE / 3=SRTP_RELAY */
    char peer_a_ip[64], peer_b_ip[64]; /* inet_ntop 打平 */
    uint16_t peer_a_port, peer_b_port;
    /* ★ v4: codec + SSRC — 之前 v3 全打 0，v4 填充有效 */
    uint8_t  codec_pt_a, codec_pt_b;   /* payload type: PCMU=0, PCMA=8, G722=9, ... */
    uint16_t codec_cr_a, codec_cr_b;   /* clock rate: 8000=PCMU/G722, 48000=Opus */
    uint32_t ssrc_a, ssrc_b;           /* relay 重写后发往对侧的 SSRC */
} repl_dialog_v4_t;
```

**v4 codec/SSRC 填充来源**：
- **SSRC**：`bridge_call_view(bm, c, &mv)` → `mv.ssrc_out[0/1]`（relay 重写后的真实出向 SSRC）
- **codec PT/CR**：`codec_registry_find_name(c->codec_a/b, strlen(name))` → `ops->default_pt` + `ops->clock_rate`
- **降级路径**：bridge_call_view 失败 / codec_registry_find_name 返回 NULL → 对应字段保持 0（和 v3 行为一致，rebuild 侧 relay 自己会重新协商）

**角色守卫**：所有接线点统一判断 `ha_role() == HA_ROLE_ACTIVE` 再 publish（ha.h `ha_repl_publish` 内部带 role guard + transport NULL 检查）。

**载荷序列化规则**：**扁平打平、禁止指针**。所有字段用 memcpy 直接传，跨版本只 bump `payload_ver` 让 apply 侧分支解析。

### 12.6 STANDBY→ACTIVE failover dialog rebuild（✅ 已实现）

role_switch_proxy 链路：ha_main 心跳仲裁 → `ha_repl_role_switch_proxy(ACTIVE)` → `b2b_repl_on_role_switch(co, ACTIVE)` → `b2b_repl_rebuild_from_buf(co)`。

**rebuild 步骤（v3 payload 完整重建）**：

```
ha_dlg_buf[256] (round-robin, 由 transport recv 线程 REPL_DIALOG apply 填充)
  │
  ├─ tbl_find(co, callid_a / callid_b) → 去重（FULL push 重发同一 dialog 防重复挂链）
  │
  ├─ mem_slab_alloc_direct(sizeof(b2b_call_t)) → new_call
  │
  ├─ leg[0/1].callid / loc_tag / rem_tag / cseq / state  ← 从 payload 填
  │
  ├─ call.active=1 / dead=0 / media_done / hangup_by / hold_legs / a_final / start_ms
  │
  ├─ sip_profile_find(dlg.prof_name) → c->prof  ← CDR 里 prof->name 不再 "(unknown)"
  │
  ├─ ★ rtp_relay_start(bm, &rl, &rp{a_rtp, b_rtp}, 0, 0)
  ├─ ★ inet_pton(peer_a_ip) → rtp_relay_set_peer_a(rl, &sa)
  ├─ ★ bridge_register_call(bm, new_call, rl, relay_mode) ← media_bridge.c 新增接口
  │    → shard[ptr_shard(new_call)].head → new cnode { call=new_call, relay=rl, mode }
  │
  └─ tbl_link(co, new_call, 0)  /* callid_a shard */
     tbl_link(co, new_call, 1)  /* callid_b shard */

后续 in-dialog BYE/re-INVITE 到同一 callid:
  tbl_find(co, callid) → rebuild 的 call (active=1, dead=0) → 正常续接 RTP
```

**bridge_register_call 新接口**：`media_bridge.c` 新增，反向绑定 call_key→relay。和 `bridge_release_call` 对称：同 `ptr_shard(call_key)` 哈希 → 同 shard 锁 → 同 malloc/free cnode 生命周期。幂等（已存在返回 `NSBC_AGAIN`）。

### 12.7 监控水位（OAM 只读）

| OAM 指标 | 含义 | 来源 |
|----------|------|------|
| `repl.published_seq` | 主端已发布最高 seq | `repl_published_seq()` |
| `repl.applied_seq` | 备端已应用最高 seq | `repl_applied_seq()` |
| `repl.gap_count` | 备端累计缺口次数 | `repl_gap_count()` |
| `repl.ring_used` | 主端环形缓冲占用 | `ha_repl` 内部 |
| `repl.lag`（计算值） | `published_seq - applied_seq` | 主端 OAM 轮询 |

当 `repl.lag > ha.replica.lag_warn`（默认 10000）时，ha_health 会把备端标记为 **DEGRADED**（不主动抢占），防备端在复制落后时接管成为数据缺失的 ACTIVE。

## 13. BFD 亚秒探测（P1 / ha_health.h）

当前 UDP 心跳每 250ms 一个仲裁周期（heartbeat_ms=500 / 4），已经能在 failover_ms（默认 3000ms）窗口内收敛故障。BFD（RFC 5880）是更轻量的亚秒级探测协议，作为心跳的 **辅助/替代** 路径，在以下场景价值最大：

| 场景 | UDP 心跳表现 | BFD 优势 |
|------|-------------|----------|
| 链路中间设备丢 UDP 保活但转发 SIP TCP | 3s 才判死 | 200ms × 3 = 600ms 即判定 |
| 对端进程存活但 UDP socket 挂住 | 永远收不到 → 3s failover | BFD Control 走独立 socket，更精确 |
| 主备跨机房专线抖动 | 丢 UDP 但 TCP 复制通 | BFD 独立探测 |

### 13.1 BFD 与 UDP 心跳的关系（✅ 已实现）

**BFD 不是替代，是补充**：

```
UDP 5070 心跳 ──→ 角色仲裁（priority/tie-break）
                    │
                    ├── 周期: heartbeat_ms（默认 500ms）
                    └── 判定: 连续 failover_ms 未收到 → 对端 Down

BFD Control ────→ 对端存活精确判定（毫秒级）
                    │
                    ├── UDP socket + 独立 pthread（ha_bfd.c）
                    ├── 周期: desired_min_tx_interval（默认 200ms）
                    ├── 状态机: AdminDown → Down → Init → Up（RFC 5881）
                    └── 判定: 连续 detect_multiplier 次未收到 → ha_hc_peer_probe(0, -1)
```

**ha_bfd.c 实现要点**：

- 独立 UDP socket（非 SOCK_DGRAM 独立于复制 TCP 5071 和心跳 UDP 5070）
- 独立 pthread `bfd_thread_main`（poll 100ms 周期）
- BFD Control 包 52 字节（AdminDown→Down→Init→Up 状态按 `local_discriminator` + `desired_min_tx_interval` 递增）
- 每收到 Control 包 → `ha_hc_peer_probe(hc, 1, rtt_ms)` 联动健康门
- 连续 `detect_multiplier` 次未收到 → `ha_hc_peer_probe(hc, 0, -1)` 标记 peer invisible
- ha.c ha_init 读 `ha.bfd.*` cfg 项 → `ha_bfd_create()`；socket/pthread 失败降级（静默跳过 BFD，心跳仲裁照常）

### 13.2 BFD Control 包格式（RFC 5880 Section 4.1）

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|Vers| Diag    |Sta|P|F| C |A|D|M|      Detect Multiplier      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Your Discriminator                      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       My Discriminator                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                   Desired Min TX Interval                       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                  Required Min RX Interval                       |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                 Required Min Echo RX Interval                   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

**本实现简化**：暂不实现 Echo 功能（D bit=0），只跑 ASCII Control 模式，52 字节/包，走 UDP。主端和备端都同时发 BFD Control，State 字段按 RFC 5881 状态机（AdminDown → Down → Init → Up）。

### 13.3 判定公式

```
BFD 探测间隔 = max(desired_min_tx_interval, required_min_rx_interval)
BFD 探测时间 = BFD 探测间隔 × detect_multiplier

示例: 200ms × 3 = 600ms failover（远快于 UDP 心跳的 3000ms）
```

### 13.4 配置 vs 运行时

| BFD 配置 | UDP 心跳 | 仲裁行为 |
|----------|----------|----------|
| enabled=true | heartbeat_ms=500, failover_ms=3000 | **BFD 600ms 即判定**，但仲裁窗口仍为 3000ms（ha.c 代码里 failover_ms 是硬阈值）。要让 BFD 生效必须同步调小 failover_ms |
| enabled=false | heartbeat_ms=500, failover_ms=3000 | 维持现状 |

> ⚠️ **关键陷阱**：启用 BFD 必须同步配置 `failover_ms = bfd_interval × detect_multiplier + 500ms`（留 UDP 心跳兜底余量），否则 BFD 探测已判对端 Down，ha.c 仲裁层还要等满 failover_ms 才升主，BFD 白开。

## 14. Hitless Upgrade 滚动升级（✅ 已实现，replica FULL push 方案）

> **重要更正**：初版文档假设用 `shm_seg.h` 跨进程共享内存段 publish 状态，但实际代码落地选择了**进程内 replica FULL push + STANDBY rebuild_from_buf** 方案。原因：更简单、无 SHM 陈旧风险、不需要新进程 attach 旧进程 SHM（新进程就是原来的 nsbcd，不是新进程）。

### 14.1 滚动升级的核心难题（及本方案解法）

| 难题 | 本方案解法 |
|------|-----------|
| **老进程如何不丢在途呼叫** | ① `ha_freeze_input(0)` 冻结新呼（b2bua INVITE gate + SOS 例外）② `ha_drain_proxy()` 轮询 in-dialog BYE 自然到期（`b2b_active_count` 遍历 shard 统计），30s 超时降级继续 |
| **新 ACTIVE 如何继承对话状态** | 不等 SHM，走 **replica FULL push**：ACTIVE 调 `ha_repl_trans_force_full_push()` → CLEAR + ring 全量 → 尾 ACK → STANDBY 侧 `accept_any_seq=1` 跳过 gap check 装完整 loc_db + dialog buf → 再 ACTIVE 切换时 `b2b_repl_rebuild_from_buf` 重建完整 call + relay |
| **如何保证只有一个 ACTIVE 在跑** | ACTIVE 主动调 `ha_hitless_upgrade()` → 内部 `g_hitless_yield_requested=1` → 心跳线程仲裁时强制 `want=STANDBY` → role flip + keepalived track_process 摘 VIP → 原 ACTIVE 进程退出 → keepalived VIP 漂到 STANDBY → STANDBY 心跳仲裁 want=ACTIVE（对端 DOWN）→ STANDBY→ACTIVE |
| **VIP 不中断** | keepalived track_process 盯着 ACTIVE nsbcd，老进程 stop 瞬间 VIP 会漂到备机。**滚动升级时序：先升备机（VIP 仍在主机）→ 再升主机（主机降 STANDBY → 进程退出 → VIP 漂备机 → 备机升 ACTIVE）** |

### 14.2 Hitless Upgrade 6 步完整流程（ha_hitless_upgrade）

```
ha_hitless_upgrade():
  │
  ├─ Step 1: FREEZE ───────────────────────┐
  │  ha_freeze_input(0)                      │
  │  g_frozen_until_ms = auto-unfreeze 120s  │  （进程被 kill 后自动解冻）
  │  b2bua INVITE gate:                      │
  │    ha_is_frozen()==1                     │
  │    && !SOS → drop                        │
  │                                           │  新呼入口关闭
  ├─ Step 2: DRAIN ──────────────────────────┤
  │  ha_drain_proxy() 200ms 轮询:            │
  │    → b2b_active_count(co)                │
  │      遍历 B2B_CALL_SHARDS                 │
  │      加锁统计 call.active==1              │
  │  等 cnt==0 或 30s 超时                    │  in-dialog BYE 自然到期
  │                                           │
  ├─ Step 3: FULL push ──────────────────────┤
  │  ha_repl_trans_force_full_push()           │
  │    → REPL_FULL_CLEAR (type=3)             │
  │    → ring 从 max(ack_seq+1, pub_seq-8192+1)│
  │      逐条重发 REPL_REG / REPL_DIALOG      │
  │    → 尾 ACK 帧 (type=0xFF, payload_len=snap_seq)│
  │  STANDBY 侧:                              │
  │    repl_set_accept_any_seq=1               │
  │    reg_repl_apply: loc_db_clear_all       │
  │    b2b_repl_apply: ha_dlg_buf clear       │  状态对齐
  │                                           │
  ├─ Step 4: yield_requested ────────────────┤
  │  atomic_store(&g_hitless_yield_requested, 1)│
  │                                           │
  ├─ Step 5: heartbeat role flip ────────────┤
  │  ha_main 仲裁周期 (250ms):                │
  │    正常 want=ACTIVE                       │
  │    yield_requested==1                     │
  │    want=STANDBY                           │  强制让位
  │    role ACTIVE→STANDBY                   │
  │    repl_trans_notify_role(STANDBY)        │
  │    repl_role_switch_proxy(STANDBY)        │
  │    log_audit("standby")                   │
  │                                           │
  └─ Step 6: 返回（1.5s 超时兜底）────────────┘
```

**后续进程操作（调用方负责）**：

```bash
systemctl stop nsbcd      # 原 ACTIVE 进程退出
                         # keepalived track_process 触发 → VIP 漂到备机
                         # 备机心跳仲裁 want=ACTIVE（对端 DOWN）→ STANDBY→ACTIVE
                         # 备机 rebuild_from_buf → 对话续接 + media anchor 重建

systemctl start nsbcd    # 原 ACTIVE 机器新进程启动
                         # 收到备机心跳 → priority 低 → 保持 STANDBY
                         # 新进程 ha_is_frozen() 默认 0（未设置） → 正常
```

### 14.3 降级安全网（稳定性 P0 硬约束）

| 场景 | 处理 | 日志 |
|------|------|------|
| **STANDBY 触发 upgrade** | 直接返回 `NSBC_NOENT` | INFO: "not ACTIVE, skip" |
| **transport 未连接**（备端离线） | 跳过 FULL push，继续 yield | WARN: "STANDBY transport not connected — skip FULL push (degrade)" |
| **upgrade 重入** | `atomic test_and_set` 防重入 | WARN: "already in progress" |
| **DRAIN 30s 超时** | WARN + 继续 FULL push + yield（不阻塞升级） | "DRAIN TIMEOUT — force yield" |
| **heartbeat 超时不 yield** | 1.5s 后返回但不强制 kill | WARN: "TIMEOUT (still ACTIVE after 1.5s)" |
| **调用方挂了（进程被 kill）** | `g_frozen_until_ms` 自动超时解冻（120s）— **永远不会永远 frozen** | WARN: "auto-unfreeze (timeout)" |
| **yield 后 keepalived 不漂 VIP** | 调用方必须 `systemctl stop nsbcd` — keepalived track_process 只盯进程存活 | — |

### 14.4 跨版本升级时序（强制顺序，防 payload_ver 不兼容）

```
时间 ──────────────────────────────────────────────────────►

节点 A (ACTIVE, 旧版本)     节点 B (STANDBY, 旧版本)     操作
─────────────────────────── ──────────────────────────── ──────────────────
  ACTIVE, 正常服务          STANDBY, 复制接收中           初始状态
                              │
                              ▼
                           升 B → B 跑 apply v3
                           （新 STANDBY apply 侧 sizeof(v3)≥sizeof(v2)
                            → 能吃旧 ACTIVE 的 v2 payload
                            → prof_name/relay 字段全 0 → rebuild 降级
                            → 但对话续接仍 OK）
                              │
                              ▼
                           ✅ B 升级完成（VIP 仍在 A，零影响）

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
                              │
  A 调 ha_hitless_upgrade     │
  ├─ FREEZE                    │   B 已升级（apply v3）
  ├─ DRAIN 30s max            │   复制链路正常
  ├─ FULL push (transport_send │
  │   _full_snapshot v3)       │
  ├─ yield_requested=1        │
  ├─ ACTIVE → STANDBY         │
  │                           │
  ▼                           ▼
  systemctl stop nsbcd       VIP 漂到 B（keepalived track_process）
  （进程退出）                 │
                              ├─ B 心跳仲裁 want=ACTIVE（对端 DOWN）
                              ├─ B 降 ACTIVE
                              ├─ b2b_repl_rebuild_from_buf(co)
                              │   → prof_find → prof 填
                              │   → rtp_relay_start + bridge_register_call
                              │     → media anchor 重建
                              │   → tbl_link
                              └─ ✅ 服务正常（对话续接 + RTP 正常）

  systemctl start nsbcd（A）
  → 收 B 心跳 → priority 低 → STANDBY
  → 正常
```

> ⚠️ **升级顺序强制：先升 STANDBY → 再升 ACTIVE**。反过来（先升 ACTIVE 到 publish v3，旧 STANDBY apply 侧 sizeof(v2 payload) 更小 → FULL push 全部 REPL_DIALOG 被 len check 跳过 → failover 后 dialog rebuild 零覆盖 — SIP 续接断）。

### 14.5 Hitless Upgrade vs 传统 SHM 方案对比

| 维度 | 本方案（replica FULL push + drain + yield） | 传统 SHM（老进程 publish → 新进程 attach） |
|------|-------------------------------------------|------------------------------------------|
| **复杂度** | 低（进程内 role flip，零跨进程通信） | 高（SHM 段创建/attach/epoch/CRC + 进程间同步） |
| **陈旧风险** | 无（STANDBY 实时接收复制，FULL push 刚对齐） | 有（SHM 被老进程 publish 后可能陈旧 → epoch/CRC 校验不过降级） |
| **代码量** | 小（~600 行：ha_hitless_upgrade + bridge_register_call + rebuild_from_buf） | 大（shm_seg + loc_db 序列化 + dialog 序列化 + attach 校验 + rebuild — 至少 1500 行） |
| **进程模型** | **同一 nsbcd 进程**降 STANDBY → stop → 新进程升 ACTIVE | **两个独立进程**（老 ACTIVE publish SHM + 新 STANDBY attach） |
| **降级链** | FREEZE→DRAIN→FULL→YIELD 每步可超时降级 | SHM attach 失败 → 新进程从零启动 loc_db（最坏 15min UA 重注册） |
| **媒体 anchor** | bridge_register_call 重建 relay（同进程内 RTPPROXY 仍在） | relay 句柄无法跨进程传递（SHM 存不了 socket fd）— 媒体必然断 |

> **结论**：对 NSBC 单进程 nsbcd 架构，本方案是**更合理的选择**。SHM 方案适合**多进程架构**（老进程只做 ACTIVE，新进程是全新的 nsbcd 实例），但当前 nsbcd 是单进程 reactor 模型 — 进程内 role flip 就够了，不需要跨进程共享。

---

**源文件**：[ha.c](../../src/ha/ha.c) · [ha_health.c](../../src/ha/ha_health.c) · [ha_replica.c](../../src/ha/ha_replica.c) · [ha_repl_transport.c](../../src/ha/ha_repl_transport.c) · [ha_bfd.c](../../src/ha/ha_bfd.c) · [registrar.c](../../src/registrar/registrar.c) · [location_db.c](../../src/registrar/location_db.c) · [b2bua.c](../../src/sip/b2bua.c) · [media_bridge.c](../../src/media/bridge/media_bridge.c)
