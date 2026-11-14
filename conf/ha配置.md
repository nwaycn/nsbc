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

## 10. P1 规划（复制 + Hitless）

当前 HA 模块的骨架已为 P1 预留接口：

| 模块 | 头文件 | 骨架完成 | 接线状态 |
|------|--------|----------|----------|
| 心跳 + 仲裁 | ha.h / ha.c | ✅ | ✅ 全接 |
| 健康门 | ha_health.h / ha_health.c | ✅ 粗骨架 | ⚠️ CPU/RSS/调度滞后已采样，peer_probe 接口已预留；**BFD/RPL 过载/复制水位接入 P1** |
| 状态复制 | ha_replica.h / ha_replica.c | ✅ **完整骨架**（seq/ring/gap/ingest/ACK/全量） | 🔲 TCP 传输层未接；**registrar.c / b2bua.c 接线点未调 repl_publish()**（见第 12 章 12.5 节） |
| BFD 亚秒探测 | ha_health.h（接口）+ P1 新增 ha_bfd.c | 🟡 接口预留（`peer_probe`/`peer_rtt_ms`） | 🔲 BFD Control socket + 状态机未接；UDP 心跳 250ms 周期已能秒级收敛 |
| Hitless Upgrade | shm_seg.h + ha.h 预留接口 | ✅ SHM 三级回退链 + epoch/CRC | 🔲 滚动升级冻结/解冻流程 + 业务 SHM publish/attach 未接（见第 14 章） |

`ha_replica` 设计要点：

- **主→备单推**，64 位单调 seq + 备端 ACK 水位
- 环形缓冲 8192 条，满了返回 NOSPC → 转全量/告警，不阻塞呼叫线程
- 缺口处理：备端期望 seq 未到 → `on_gap` 回调请求重传
- 载荷类型：REPL_REG / REPL_DIALOG / REPL_CDR_FLUSH / REPL_FULL
- 热升级跨版本：payload 带 `ver` 字段

接线点在 **registrar.c**（注册增删）和 **b2bua.c**（对话锚点增删）——调用 `repl_publish()` 即可。

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

### 12.3 备端应用与缺口重传

`repl_ingest()` 判定逻辑（ha_replica.c L72-L96 已实现）：

| 收到的 seq | 期望 seq = applied_seq + 1 | 处理 |
|-----------|---------------------------|------|
| ≤ applied_seq | — | 幂等丢弃（重传重复） |
| == 期望 | ✅ | 调 `ops.apply()` → `applied_seq = rec->seq` |
| > 期望 | ❌ 缺口 | `gaps++` → `ops.on_gap(expect, got)` → 返回 `NSBC_AGAIN` |

**缺口处理策略**：

1. `on_gap` 回调备端向主端请求重传：发一个 **REPL_GAP_REQ** 帧（type=0xFE，payload 含 `expect_seq..got_seq-1` 区间）；
2. 主端收 GAP_REQ 后按区间从环形缓冲取出重发；
3. 若缺口范围超环形缓冲容量（备端长时间离线），主端直接回推 `REPL_FULL` 全量快照；
4. 骨架版本不做乱序缓存，收到缺口直接等重传（粗实现）；正式 P1 接线时可加一个 256 条的乱序窗口 `pending[seq & 0xFF]`。

### 12.4 全量快照（REPL_FULL）

**触发条件**：

| 触发源 | 条件 |
|--------|------|
| 环形缓冲满 | 主端 `repl_publish()` 返回 NOSPC → 主端主动发 FULL |
| 缺口请求超界 | 备端 `on_gap` 请求的区间超出主端环形缓冲 |
| 周期对齐 | `ha.replica.full_resync_interval` 配置（默认 0=关闭） |
| 主备切换后首次对齐 | 新 ACTIVE 启动后立即推一次 FULL |

**FULL 载荷格式**（主端序列化）：

```
[64 位 snapshot_seq]           ← 对齐水位
[32 位 type_count]             ← 条目数
[条目1: REPL_REG 格式]
[条目2: REPL_DIALOG 格式]
...
```

备端 `repl_resync_done(r, snapshot_seq)` 推进 applied_seq + 清零 gaps。

### 12.5 接线点（业务模块调用 repl_publish）

| 模块 | 事件 | 载荷格式（建议） | 调用时机 |
|------|------|-----------------|----------|
| [registrar.c](../../src/registrar/registrar.c) | REGISTER 200 OK（新增/刷新绑定） | `REPL_REG { aor, contact, expires }` | `loc_upsert()` 成功后（ACTIVE 角色） |
| [registrar.c](../../src/registrar/registrar.c) | REGISTER 4xx/expire=0（注销） | `REPL_REG { aor, contact, expires=0 }` | `loc_upsert()` 返回 expires=0 或 `loc_del()` |
| [b2bua.c](../../src/sip/b2bua.c) | INVITE accept（对话锚点创建） | `REPL_DIALOG { call_id, cseq, media_addr, state }` | `b2b_accept()` 成功（ACTIVE 角色） |
| [b2bua.c](../../src/sip/b2bua.c) | BYE final（对话清理） | `REPL_DIALOG { call_id, state=TERMINATED }` | `b2b_bye_final()` |

**角色守卫**：所有接线点统一判断 `ha_role() == HA_ROLE_ACTIVE` 再 publish，STANDBY 跳过（防双写）。

**载荷序列化**：业务模块自带版本号（`payload_ver`），结构演进时 bump 版本号即可，备端按版本号分支解析。**禁止直接 memcpy 含指针对象**（跨进程/跨地址空间指针无意义）。

### 12.6 监控水位（OAM 只读）

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

### 13.1 BFD 与 UDP 心跳的关系

**BFD 不是替代，是补充**：

```
UDP 5070 心跳 ──→ 角色仲裁（priority/tie-break）
                    │
                    ├── 周期: heartbeat_ms（默认 500ms）
                    └── 判定: 连续 failover_ms 未收到 → 对端 Down

BFD Control ────→ 对端存活精确判定（毫秒级）
                    │
                    ├── 周期: desired_min_tx_interval（默认 200ms）
                    └── 判定: 连续 detect_multiplier 次未收到 → 对端 Down
                              （经典 BFD: interval × multiplier 即探测时间）
```

**ha_health.h 接口已预留**：

```c
void ha_hc_peer_probe(ha_hc_t *h, int ok, int rtt_ms);
int  ha_hc_peer_visible(ha_hc_t *h);
int  ha_hc_peer_rtt_ms(ha_hc_t *h);
```

当前 UDP 心跳每周期调一次 `ha_hc_peer_probe(got_hb, 0)`。BFD 启用后，心跳线程 **额外** 起一个 BFD Control socket，每 `desired_min_tx_interval` 发 BFD Control 包，每收到一个调 `ha_hc_peer_probe(1, rtt_ms)`；连续 `detect_multiplier` 次没收到则调 `ha_hc_peer_probe(0, -1)`。

仲裁层（ha.c）只读 `ha_hc_peer_visible()` 的结果，不关心底层是 UDP 还是 BFD。

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

## 14. Hitless Upgrade 滚动升级（P1 / shm_seg.h）

> 代码骨架：[shm_seg.h](../../src/include/shm_seg.h) 已实现 `memfd_create → POSIX shm_open → SystemV shmget` 三级回退链、`epoch` 世代号、`CRC32C` 校验。本章节描述"老进程出、新进程进"的滚动升级流程。

### 14.1 滚动升级的核心难题

| 难题 | 解法 |
|------|------|
| **老进程如何不丢在途呼叫** | 冻结输入（`FREEZE` 控制面）→ 等在途对话 drain → 关闭 |
| **新进程如何继承注册数据** | 共享内存段（`shm_seg`）：老进程 publish 到 SHM，新进程 attach + 校验 epoch/CRC 加载 loc_db 热启动 |
| **如何保证只有一个 ACTIVE 在跑** | 老进程主动让位 STANDBY → 新进程启动后心跳仲裁 → 新 ACTIVE 接管 |
| **VIP 不中断** | keepalived track_process 盯着 ACTIVE nsbcd，老进程 stop 瞬间 VIP 会漂到备机（如果备机也停了）。正确做法是 **滚动升级时先升备机，再升主机**（见 14.3） |

### 14.2 滚动升级时序（先升备机，再升主机）

```
时间 ──────────────────────────────────────────────────────►

节点 A (ACTIVE, 旧版本)     节点 B (STANDBY, 旧版本)     操作
─────────────────────────── ──────────────────────────── ──────────────────
  ACTIVE, 正常服务          STANDBY, 复制接收中           初始状态
                              │
                              ▼
                           B 收到 systemctl upgrade
                              │
                              ▼
                           发 HA_REPL_FULL → 对端 A 立即发全量快照
                              │
                              ▼
                           detach 旧 nsbcd（进程退出）
                              │
                              ▼
                           新 nsbcd 启动 → attach SHM (nsbc_ha_node-b)
                           → 加载 loc_db / dialog snapshot（校验 epoch/CRC）
                           → 心跳启动 → 收到 A 心跳 → STANDBY 正常
                              │
                              ▼
                           ✅ B 升级完成（VIP 仍在 A，零影响）

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
                              │
  A 收到 systemctl upgrade     │
  ├─ ① 发 FREEZE 控制面          │
  ├─ ② 拒绝新 INVITE（已实现）   │   备机 B 已升级，复制链路正常
  ├─ ③ wait all in-dialog BYE │
  ├─ ④ publish final snapshot ─┼─→ SHM (nsbc_ha_node-a)
  ├─ ⑤ detach 旧 nsbcd          │
  ├─ ⑥ B 心跳判定 A Down        │   failover_ms 内 B 升 ACTIVE
  │                              │
  ▼                              ▼
  新 nsbcd 启动 → attach SHM     VIP 漂到 B（keepalived track_process）
  → 加载 loc_db / dialog         → B 开始接呼
  → 心跳启动 → 收到 B 心跳       → 按 priority 降 STANDBY（或等待 A 完成）
  → STANDBY 正常                 → 服务正常

  ✅ A 升级完成
```

### 14.3 共享内存段设计（shm_seg.h）

**段结构**：

```
┌───────────────────────────────────────┐
│ shm_hdr_t (48 字节)                   │
│   magic      = 0x4e534243 ("NSBC")    │
│   version    = 1                      │
│   size       = 整个段大小              │
│   epoch      = 每次 publish +1         │  ← 新进程 attach 后读 epoch，
│   crc        = payload CRC32C         │     若与上一代相同则拒绝（陈旧 SHM）
│   reserved   = 4 × 8 字节              │
├───────────────────────────────────────┤
│ loc_db 快照（数组 or 哈希表序列化）     │
├───────────────────────────────────────┤
│ dialog 快照                            │
├───────────────────────────────────────┤
│ ... (预留扩展)                         │
└───────────────────────────────────────┘
```

**SHM 创建/attach 判定**：

```c
shm_seg_t *seg = shm_seg_open(&(shm_cfg_t){
    .name    = "nsbc_ha_node-a",     // prefix + hostname
    .size    = 256 * 1024 * 1024,    // 256MB 可配置
    .create  = 1,                    // ACTIVE 创建；STANDBY attach
    .backend_force = SHM_BACKEND_NONE, // memfd → POSIX → SysV 回退
}, &seg);

// 新进程 attach 后校验：
int rc = shm_validate(seg, &epoch, &crc);
if (rc != NSBC_OK || epoch <= last_seen_epoch)
    // 版本不匹配或 SHM 已陈旧 → 从零启动 loc_db（降级）
```

### 14.4 冻结/解冻控制面

**控制命令**：`nsbcli ha freeze [ms]` 或通过 UDS `control` 接口。

```c
// ha.h 预留（待 P1 接线）
void ha_freeze_input(int freeze_ms);  // 冻结新输入 ms；0=永久直到 unfreeze
void ha_unfreeze_input(void);
int  ha_is_frozen(void);
```

冻结期间：
- INVITE 入站：备机继续丢弃（正常门控），主机拒绝新 INVITE（**新增门控**，当前 P0 未实现）；
- REGISTER 入站：继续受理（保持 loc_db 热备）；
- 外呼：继续拒绝（已实现）。

**关键：冻结后不立即退出**。要等所有 in-dialog 对话自然结束（BYE/CANCEL），或等到 `upgrade_timeout_ms`，才真正 detach 进程。

### 14.5 降级与安全网

| 场景 | 处理 |
|------|------|
| SHM attach 失败（旧版无段、或版本不匹配） | 新进程从零启动 loc_db → 等 UA 下次 REGISTER 刷新（最坏 15min），HA 正常服务但丢失在途注册 |
| FREEZE 后在途呼叫长期不结束 | `upgrade_timeout_ms`（默认 10s）到期后强制 detach（会丢在途呼叫，日志 WARN） |
| 老进程 detach 瞬间 VIP 飘回旧 ACTIVE | keepalived track_process + 进程级 watchdog 兜底：进程已退出 → track_process 自动摘 VIP |
| 新进程启动太慢 | `upgrade_timeout_ms` 到期后 keepalived 重新漂 VIP（但此时老进程已死）→ **需要提前 500ms 调大 failover_ms 留冗余窗口** |

---

**源文件**：[ha.c](../../src/ha/ha.c) · [ha_health.c](../../src/ha/ha_health.c) · [ha_replica.c](../../src/ha/ha_replica.c) · [shm_seg.h](../../src/include/shm_seg.h) · [registrar.c](../../src/registrar/registrar.c) · [b2bua.c](../../src/sip/b2bua.c)
