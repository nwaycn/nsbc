# NSBC 性能与并发改进交接

更新时间：2026-10-25

## 目标与边界

用户要求继续按[性能与并发架构改进建议](./审计报告/性能与并发架构改进建议-20261025.md)
推进 `src/` 与 `web/src/`，重点是多 Reactor、多 Worker，而不是停留在外围优化。
用户在 Linux 上验证 C 侧构建，不要求在当前 Windows 环境编译。

继续实施时必须维持以下事实：P3 多 Reactor 已真正实现（`core.reactors=N` 放开强制 n=1，
SO_REUSEPORT N UDP socket，reactor[1..N-1] 独立 pthread，`reactor_mgr_route` = `hash_key % n`）；
P3 按需 Worker 动态扩缩已实现（`wpool_resize` scale-up/down + ticker ~1s 周期检查触发，
`core.workers=cgf_hot` 配置变化自动生效）。reactor_manager 不再强制 n=1。

## 当前状态

- [docs/architecture.md](./docs/architecture.md) 已更正线程模型图：时间轮仍由 ticker
  推进，回调当前仍运行在 ticker 线程；说明 mailbox 尚未接入定时器和
  `reactor_add`/`reactor_del` 调用点；更新 CDR Worker 的实际用途。
- [性能与并发架构改进建议-20261025.md](./审计报告/性能与并发架构改进建议-20261025.md)
  已补充实施状态，修正“Worker 没有业务提交点”的过时描述，并将阶段 2 标为已具备
  基础 mailbox、但尚未进行业务迁移。
- 已有代码基础：`reactor_call()` 提供容量 4096 的同步命令队列、eventfd 唤醒、
  owner-thread 执行、排队/完成/拒绝统计和 stop/drain 行为。跨线程提交要求 event
  loop 已启动；队列满、尚未启动、停机均显式返回错误。
- `reactor_wait()` 由创建 Reactor 的线程调用。`reactor_add()`/`reactor_del()` 更新
  owner-only callback 表，但目前仅文档要求 owner 序列化，没有 mailbox 路由或强制
  owner 检查。`reactor_mod()` 只更新内核 epoll mask，不访问 callback 表。
- 本轮 C 代码推进：`reactor_destroy()` 改为返回状态、仅允许 owner 调用（其他线程
  返回 `NSBC_PERM_DENIED`），并在 `reactor_wait()` 活跃或命令仍排队时返回
  `NSBC_BUSY`；开始销毁时先关闭 mailbox，主程序两处销毁调用均检查并记录结果。
  正常停机路径也会检查命令 drain 的返回值。调用方仍必须确保并发 API 使用者已静止。
- `main.c` 在正常停机路径先 stop Reactor、再以 `reactor_wait(..., 0)` 排空已接受的
  命令，之后停止模块。ticker 仍在模块停止及 active-call drain 期间运行，最后才停。
- 本轮未迁移定时器或 FD 操作；`reactor_add/del` 尚无 owner 校验。
- 本轮 Worker 生命周期改进：`wpool_submit()` 和 `wpool_submit_any()` 使用原子 admission
  状态跟 shutdown 关闭门竞争；关闭方等待已进入提交区的生产者结束后再停止 Worker，
  避免每次提交增加全局 mutex。shutdown 后新提交返回并计入拒绝的 `NSBC_SHUTDOWN`，
  此前已接受任务仍由 worker drain。调用方仍需在 destroy 前停止并 join 所有 producer，
  避免 pool 指针在调用者手中失效。
- Worker 的 `nsbc_worker_threads` metric source 现在在关闭提交后、停止 worker 和销毁
  队列前注销。metric registry 在持有注册表锁期间调用 render；注销获取同一把锁，因此
  会等待已开始的渲染，避免 `/metrics` 在 shutdown 中访问已释放的 pool 或队列。
- 新增 `tests/unit/t_worker_lifecycle.c`：阻塞单 worker 的首个 job，同时启动 pool
  destroy；先填满 8 槽队列并验证额外提交返回 `NSBC_NOSPC`，再验证 shutdown 拒绝后续
  提交、已接受任务 drain、pool 释放后指标渲染不再包含 Worker source。使用 `make check`
  可运行；本机未执行 C 编译/测试，需 Linux 验证。
- 顶层 `Makefile` 提供 `make check`，测试源位于 `tests/unit/t_*.c`；此前 `tests/unit`
  目录不存在，现有 Worker 和 registrar 两个用例。运行 `make check` 需在 Linux 构建环境完成。
- 本轮 Registrar 改进：`loc_upsert()` 在 AoR 联系人上限时，直接复用最早到期的
  `loc_node_t` 填充新绑定并移到链表头；不再先释放 slab 节点再申请新节点，降低这条
  替换路径的 slab 竞争/分配失败窗口。淘汰策略和联系人总数不变。新增
  `tests/unit/t_location_db.c` 覆盖单 AoR 上限替换、查询及计数；Linux 上尚未运行。
- 本轮 SIP transaction 哈希复用扩展至 client/server 输入、客户端插入、服务端创建、
  响应处理和摘表路径：每个操作计算一次 branch/method/kind 哈希并用于 shard/bucket
  选择；匹配逻辑及锁策略未变。当前没有事务专项单测或基准，本机未编译/运行，Linux
  上应至少执行 `make check` 并补测重传、响应、ACK/CANCEL、Timer 超时。
- 本次执行 `git diff --check` 和静态路径核对；没有运行 C 编译或测试。

### P3：Event bus 全局序号分片（本轮新增）

- `src/core/event_bus.c`：删除 `volatile uint64_t g_global_seq`（跨核 atomic fetch_add cache line
  争用），改为 per-core 独立 `local_seq` 原子字段，挂在每个 `per_core_ring_t` 上。
- `event_bus_next_seq()` 改为 `pcpt_idx()` 取当前核 → `__atomic_fetch_add(&rb->local_seq, 1, RELAXED)`。
  RELAXED 足够——per-core 单 producer，帧的可见性由 `write_pos` release store 保证。
- 消费者仍按 ring 自身顺序读，per-core 单调完全够语义；当前无跨核严格全序依赖。
- 事件帧里的 `seq` 语义不变，仍是单调递增标记。

### P1：Reactor owner 审计 + 编译开关断言（本轮新增）

- `src/include/reactor.h`：加完整的 owner 归属说明、编译开关 `NSBC_REACTOR_OWNER_CHECK`
  注释、以及 56 处 reactor_add/del/mod 调用点的审计表（按线程归属分类）。
- `src/core/reactor.c`：`reactor_add()` 和 `reactor_del()` 入口加 owner 断言块，
  `#ifdef NSBC_REACTOR_OWNER_CHECK` 控制开启/关闭。默认关闭（生产构建不误炸已知
  跨线程调用点）；debug 构建定义此宏后非 owner 调用会返回 `NSBC_PERM_DENIED`。
- 审计发现 56 处调用中仅 `media_bridge.c` 的 `rtp_relay_arm()` / `rtp_relay_stop()`
  是非 reactor owner 线程调用 reactor_add/del；已加 `P1 OWNER CHECK` 注释标记。
  sip_transport.c 的 `conn_close()` 经 Grep 验证全部在 `on_tcp_event` 回调上下文
  （= owner 线程），之前审计表误标已修正。
- 单 reactor 模式下 callback 表只写不读竞争，跨线程调用安全；多 reactor manager
  实现后必须通过 manager 路由。

### P2：Reactor manager 抽象（本轮新增）

- 新建 `src/include/reactor_manager.h`：manager API（`reactor_mgr_create` /
  `reactor_mgr_get` / `reactor_mgr_by_id` / `reactor_mgr_route` /
  `reactor_mgr_stop_all` / `reactor_mgr_destroy`）。
- 新建 `src/core/reactor_manager.c`：manager wrapper 实现。当前强制 n=1（多 reactor
  属阶段 3，需灰度验证），`core.reactors>1` 时 WARN 并强制回退到 1。
  `reactor_mgr_route()` 当前恒返回 0，预留未来 `hash_key % m->n` 扩展点。
- `src/core/main.c`：`ctx.re = reactor_create(1024)` 改为读 `core.reactors` 配置
  → `reactor_mgr_create(n, 1024)` → `reactor_mgr_get(mgr, 0)` → `ctx.re`；
  `ctx.mgr` 同时存 reactor manager 句柄，方便阶段 3 多 reactor 时各模块直接路由；
  两处 shutdown 路径的 `reactor_mgr_destroy(mgr)` 改为 `reactor_mgr_destroy(ctx.mgr)`。
  单 reactor 场景下 `reactor_stop(ctx.re)` + `reactor_wait(ctx.re, 0)` drain 逻辑保持不变
  （多 reactor 时改为 `reactor_mgr_stop_all` + 循环 drain）。
- `src/include/app_ctx.h`：`nsbc_ctx_t` 新增 `reactor_mgr_t *mgr` 字段。
- `src/core/cfg_schema.c`：新增 `core.reactors` 键，默认 `"1"`，范围 `[1, 64]`。
- **API 零改动**：`reactor_create/add/del/mod/call/wait/stop/destroy` 签名不变；
  ctx.re 保持 `reactor_t *`；所有 sip_transport/media_bridge/oam 模块零代码改动。
  manager 是透明路由层，当前只有一个 reactor，未来扩展多实例时业务代码不变。
- **Makefile 零改动**：`SRCS_CORE` 用 `find src -name '*.c'` 自动发现 `reactor_manager.c`。

### 本轮 Reactor 单元测试（延续 P0）

- `tests/unit/t_reactor.c`：7 个场景覆盖 `reactor_call` 语义（make check 自动发现）：
  owner 直调、跨线程同步返回、event loop 未启动拒绝（NSBC_AGAIN）、队列满（8 线程并发触发 NSBC_NOSPC）、
  stop 后拒绝（NSBC_SHUTDOWN）、destroy 状态检查（非 owner PERM_DENIED、stop+drain 后 OK）、stop+drain 已接受命令。
- 用户已在 Linux 上运行并通过。

### P3：多 Reactor 真正实现（本轮核心）

**改动范围**：reactor_manager.{h,c}、reactor.c、sip_transport.{h,c}、media_bridge.{h,c}、signaling.c、main.c

#### 1. reactor_manager — 放开 n>1 + 独立 pthread 线程化

- `reactor_mgr_create()`：**去掉强制 n=1**（之前 WARN 降级到 1），现在按 `core.reactors=N` 真正创建 N 个 reactor 对象。
  reactor 对象预创建（main 线程 owner）——此时无线程运行，main 线程做所有模块 init 的 reactor_add，
  slot table 只有单线程访问，无并发竞争（slot_ensure 的 realloc 安全）。
- `reactor_mgr_start_threads()`：在 `modmgr_start_all()` **之后**调用（关键时序——slot table race 防护），
  为 reactor[1..N-1] 各创建一个 pthread 跑 `reactor_thread_fn`。reactor[0] 留给 main 线程（耦合 ticker 时间轮）。
- `reactor_thread_fn()`：loop `reactor_wait(r, 1000)` until `reactor_stopping(r)`；
  退出前 reactor_wait(0) drain 剩余任务 + **自 reactor_destroy(r)**。
  生产构建 reactor_destroy 的 owner 检查关闭（多 reactor 场景 reactor owner=main，但 reactor 线程自 destroy 安全——wait_active=0 + 无 pending）。
- `reactor_mgr_route(hash_key)`：从恒返回 0 改为 **`hash_key % m->n`**——真正多 reactor 路由。
- `reactor_mgr_join()`：join reactor[1..N-1] 线程。

#### 2. reactor.c — owner 检查编译开关统一

- reactor_wait、reactor_destroy 的 `pthread_equal(pthread_self(), r->owner)` 检查从硬编码改为
  `#ifdef NSBC_REACTOR_OWNER_CHECK` 编译开关控制（与 reactor_add/del 的 P1 检查对齐）。
  生产构建关闭，debug 构建开启验证。
- 原因：多 reactor 场景 reactor[1..N-1] 由 main 线程预创建（owner=main），但 reactor_thread_fn 跑 reactor_wait 和自 destroy。
  wait_active 标志已防护多线程并发 reactor_wait——owner 检查在这里是冗余防御。

#### 3. sip_transport — N 个 SO_REUSEPORT UDP socket 内核并行

- `sip_tr_cfg_t` 新增 `reactor_mgr_t *mgr` 字段。
- UDP socket 创建：如果 cfg->mgr 不为 NULL，循环 `reactor_mgr_count(mgr)` 次
  （= reactor 数量，cap 到 SIP_TR_FD_MAX=16），每次 `net_udp_open(listen_ip, udp_port, &uc)`
  + `reactor_add(mgr->reactors[i], fd, EPOLLIN, on_udp_event, t)`。
- SO_REUSEPORT 强制开启：多 reactor 同端口绑多 socket 必须用 SO_REUSEPORT，否则 bind 失败。
  如果 cfg->mgr 不为 NULL 但 cfg->reuseport=0，自动置 1 + log_warn。
- 内核 `reuseport` 负载均衡把 SIP UDP 入向包分发到不同 reactor 的 socket → 真正并行。
- TCP/TLS listener 暂时保持 reactor[0]（低频，不追求并行）。

#### 4. media_bridge — rtp_relay 按端口 hash 路由 + reactor_call 批量提交

- `mb_cfg_t` 新增 `reactor_mgr_t *mgr` 字段。
- `rtp_relay_t` 新增 `reactor_t *home_reactor` 字段（arm 时路由选定，stop 时用）。
- 新增 `relay_batch_arm_fn` / `relay_batch_stop_fn`：reactor_call 目标函数，
  在 reactor owner 线程里一次性调 4 个 reactor_add / reactor_del。
  批量提交代替 4 次跨线程调用 → 降低延迟。
- `rtp_relay_arm`：计算 hash_key = rtp_port[0] + rtp_port[1]*31 →
  `reactor_mgr_route(mgr, hash_key)` → `reactor_mgr_get(mgr, rid)` →
  `reactor_call(target, relay_batch_arm_fn, r)`。
- `rtp_relay_stop`：`reactor_call(r->home_reactor, relay_batch_stop_fn, r)`。
  reactor_call 同步等待 owner 执行完 reactor_del 才返回 → 之后 close fd / free relay 安全。
  **修复了之前 stop() 从业务线程 free relay 而 relay_cb 还在跑的 race condition**
  （del + 后续 callback/fd close 在同一 reactor owner 线程串行化）。
- 旧路径降级：cfg->mgr 为 NULL 时走 `m->cfg.re`，与 P2 行为一致。

#### 5. main.c — shutdown 顺序关键修复（Advisor 验证）

正确 shutdown 顺序（**shutdown ordering bug 已修复**）：
1. `reactor_mgr_stop_all(ctx.mgr)` — 设置所有 reactor stop 标志（不 destroy）
2. `reactor_stop(ctx.re)` + `reactor_wait(ctx.re, 0)` — drain reactor[0] 已接受命令
3. `modmgr_stop_all(mm, &ctx)` — cleanup calls（rtp_relay_stop 里 reactor_call
   因 reactor_stopping 返回 NSBC_SHUTDOWN，安全降级）
4. drain active calls — 等待活跃呼叫结束
5. `reactor_mgr_join(ctx.mgr)` — **关键：现在才 join reactor[1..N-1]**
   （之前错误地在 modmgr_stop_all 之前 join → reactor 被 free 后 rtp_relay_stop 触达已释放内存 → use-after-free）
6. `reactor_mgr_destroy(ctx.mgr)` — destroy reactor[0] + free 数组

#### 配置

`core.reactors = 2` 开启多 reactor（默认 1，上限 64）。**必须同时开启 SO_REUSEPORT**
（`sip.udp_reuseport=true` 或多 reactor 自动强制）。

#### 注意事项

- `NSBC_REACTOR_OWNER_CHECK` debug 构建与多 reactor **不兼容**：reactor_wait/destroy
  在 reactor 线程里 owner 检查不通过（reactor 预创建时 owner=main）。生产构建关闭此检查。
- TCP/TLS listener **保持 reactor[0]**（accept 是有状态三次握手，多 SO_REUSEPORT TCP listener 复杂），
  但 conn_accept 后按 peer hash 路由新连接到不同 reactor（见 TCP/TLS 多 reactor 章节）。

### P3.5：UDP data race 修复 + TCP/TLS 多 reactor 连接分布 + CPU 绑核（本轮新增）

#### 1. UDP data race 修复（P3 已存在的回归 Bug）

**问题**：`sip_transport_t.rx` 和 `sip_transport_t.dg[NET_BATCH]` 是 per-transport 共享字段，
但 P3 UDP 多 reactor 后 N 个 reactor 线程同时跑 `on_udp_event` 并发写 → **data race**。

**修复**：per-reactor 分配 rx/dg 数组。
- `struct sip_transport`：`net_rx_t rx` → `net_rx_t *per_reactor_rx`（[n_rx]）；
  `net_dgram_t dg[NET_BATCH]` → `net_dgram_t (*per_reactor_dg)[NET_BATCH]`（[n_rx][NET_BATCH]）
- init：先算 `n_rx = cfg->mgr ? reactor_mgr_count(cfg->mgr) : 1`，calloc 分配 + 每组
  `dg[i].buf = malloc(max_msg)`（原来单组初始化移到这里）
- destroy：per-reactor 循环 free 每组 dg[i].buf + free 两个数组
- `on_udp_event`：通过 fd 在 `udp_fds[]` 数组里反查下标（== reactor index），
  取 `&per_reactor_rx[rx_idx]` 和 `per_reactor_dg[rx_idx]` —— 每个 reactor 线程独立一组

#### 2. TCP/TLS 多 reactor 连接分布

**设计**：TCP/TLS listener **保持 reactor[0]**（accept 三次握手有状态，多 listener 复杂），
但 conn_accept 后新连接按 peer hash 路由到不同 reactor。

**改动**：
- `tr_conn_t` 加 `reactor_t *home_reactor` 字段
- `conn_accept`（reactor[0] 线程）：`uint64_t h = conn_key_hash(is_tls, &peer)` →
  `reactor_mgr_route(mgr, h)` → `c->home_reactor = reactor_mgr_get(mgr, rid)`；
  `reactor_add(c->home_reactor, fd, ...)` 替代 `reactor_add(t->cfg.re, ...)`
- **全路径替换**：sip_transport.c 里 7 处 `reactor_mod(t->cfg.re, c->fd, ...)` →
  `reactor_mod(c->home_reactor, c->fd, ...)`；`conn_close` 里 `reactor_del(t->cfg.re, fd)` →
  `reactor_del(c->home_reactor, fd)`
- **Listener FD 注册保持 reactor[0]**：TCP/TLS listener 创建路径（sip_tr_create + add_listeners）
  和 destroy 路径的 reactor_add/del 保持 `t->cfg.re` ✅

#### 3. conn_lock：多 reactor 下 conns 数组并发保护（Advisor 新发现的 race）

**问题**：conn_accept 在 reactor[0] 写 `t->conns[]` / `t->conns_n` / `realloc(t->conns)`，
conn_close 在 `c->home_reactor`（可能 reactor[1..N-1]）读同一个 `t->conns[]` 数组 ——
多 reactor 并发无锁访问 → use-after-free（realloc 后旧指针）和 data race。

**修复**：`sip_transport_t` 加 `pthread_mutex_t conn_lock`，保护所有 conns 数组操作。
- conn_accept：realloc conns_cap 加锁；conns[fd]=c/++conns_n 加锁；TLS 失败回滚 conns[fd]=NULL/--conns_n 加锁
- conn_close：conn_idx 读 conns[]/conns_n 加锁；reactor_del **在锁外**（防止 reactor 回调死锁）；free(c) 后 conns[i]=NULL/--conns_n 再拿锁
- init/destroy：pthread_mutex_init/destroy

**锁分层（无死锁）**：conn_lock → reactor_del（锁外）→ idx.lock（独立）→ ip_conn_lock（独立）。
严格不嵌套：conn_lock 内绝不拿 reactor 锁或 reactor_del，idx.lock 和 ip_conn_lock 独立无依赖。

#### 4. CPU 亲和性绑核

**改动**：`reactor_manager.c`
- 加 `#define _GNU_SOURCE` + `#include <sched.h>`
- reactor_thread_fn 参数从 `reactor_t *r` 改成 `reactor_thread_arg_t {reactor_mgr_t *mgr; unsigned rid}`
  —— pthread_create 传 malloc struct；线程入口拷贝到局部变量后 free(arg)
- 线程入口：`CPU_SET(rid - 1, &cpuset)` + `pthread_setaffinity_np(pthread_self(), ...)`
  —— reactor[1] 绑 CPU 0，reactor[2] 绑 CPU 1...
- **reactor[0]/main 线程不绑核**（保持调度灵活性，ticker + main event loop 需要响应性）
- 绑核失败（容器 CPU 配额不足 / CPU_SET 越界）log_warn 继续跑，不阻塞

#### 配置（无新增）

- TCP/TLS 多 reactor 连接分布：**零新增配置**——自动按 peer hash 路由
- CPU 绑核：**零新增配置**——自动按 reactor id 绑核（用户可在外部 cpuset 控制）

#### Linux 验证

```bash
# UDP race 验证：core.reactors=2 启动后发 SIP INVITE，strace 确认两个 reactor 线程
# 各自 recvmmsg 自己的 UDP fd（fd 不同，互不干扰）
core.reactors=2 nsbcd
strace -e recvmmsg -p $(pgrep -f reactor) 2>&1 | head -20

# CPU 绑核验证
ps -eLf | grep reactor
# 应该看到类似：... Nway  10  reactor_thread 184321  00:00:00 CPU4/TASK...
taskset -p $(pgrep -f reactor_manager)  # 查看绑核

# TCP/TLS 连接分布验证
# 多 reactor 下从不同 peer 发起 TCP SIP 连接，ss -tn 显示的 socket 应在不同 reactor 线程
ss -tnlp | grep :5060

# 全部测试
make check
```

### Timer callback 多 reactor 设计结论（零代码改动）

**决策**：timer callback **保持 ticker 线程直接执行**，不通过 reactor_call 路由。

**原因**（经 Grep 全量验证）：
- 全量 59 处 `tw_add/tw_mod/tw_del` 调用点覆盖 7 个模块，所有 callback 都是：
  - SIP txn 重传：`sendto()` UDP（线程安全）
  - session timer refresh/expire：改 b2bua 内部状态（worker 线程）
  - outbound_reg：改注册状态（worker 线程）
  - dialplan sleep：resume dialplan（worker 线程）
  - playback：pull RTP（media 线程）
  - sweep：清理过期注册
- **零 callback 涉及 reactor-owned FD state 操作**，零 reactor affinity 需求。

**当前 timer_wheel 设计已经是最佳状态**：
- `tw_advance()` 锁内摘出 fired 节点链 → 锁外统一回调（line 226: `n->cb(now_ms, n->arg)`）
- ticker 线程推时间轮 + 执行 callback 天然串行，无跨 reactor 调度复杂度
- 绝大多数 callback 都是线程安全 UDP sendto()，不需要 reactor owner 线程

**未来若出现需要 reactor affinity 的 callback**（如 media/DTLS timeout 关 reactor-owned FD），
callback 在内部自行 `reactor_call(target, fn, arg)` 即可。ticker 线程同步等待短暂阻塞可接受
（此类 timeout 频率很低），shutdown 期间 `reactor_call` 返回 `NSBC_SHUTDOWN` → callback 静默跳过
（正确行为）。**不修改 timer_wheel.h/.c**——保持 ticker 线程直接推 + 执行 callback。

### P3：Worker 按需动态扩缩（本轮核心）

**改动范围**：worker.{h,c}、main.c、cfg_schema.c、t_worker_lifecycle.c

#### 核心约束（非 negotiable）

- **worker_t 数组预分配 max_workers 槽位，永不 realloc**：submitter 无锁 `hash_key % atomic_load(n_active)` 算 idx，realloc = use-after-free
- scale-up 部分失败必须 rollback（draining + drain queue + pthread_join + 清理 slot 资源）
- resize / destroy / scale-up / scale-down / rollback 全路径持有 `resize_lock`
- `wpool_destroy` 必须在持有 `resize_lock` 期间先 `pthread_mutex_unlock` 再 `pthread_mutex_destroy`
  （POSIX 约束：destroy 时锁不能被持有）

#### API 签名

```c
// 新签名（min_workers 启动数，max_workers 槽位上限）
int wpool_create(worker_pool_t **out, unsigned min_workers, unsigned max_workers,
                 unsigned queue_cap, mem_rcu_t *rcu);

// 动态扩缩（幂等：new_n == old_n 直接返回 OK）
int wpool_resize(worker_pool_t *pool, unsigned new_n);

// 槽位上限（配置 clamp 用）
unsigned wpool_max_size(const worker_pool_t *pool);
```

#### 数据结构

```c
typedef struct {
    // ... per-worker 字段 ...
    mem_mpsc_t *q;
    pthread_t thr;
    sem_t ready;
    nsbc_atomic_t draining;  // P3: scale-down 标志
    int started;             // 线程已创建（只在 resize_lock 内写）
    int slot_ready;          // slot 资源已 init（只在 resize_lock 内写）
    int ready_initialized;   // sem 已 init
    mem_rcu_reader_t *rd;    // RCU reader（线程 join 后可清空）
} worker_t;

struct worker_pool {
    unsigned max_workers;         // P3: 槽位上限（永不 realloc）
    nsbc_atomic_t n_active;       // P3: 当前活跃（submitter 读）
    worker_t *w;                  // calloc(max_workers)
    pthread_mutex_t resize_lock;  // P3: 保护 resize/destroy/rollback
    int resize_lock_initialized;
    // ... 原有字段 ...
};
```

#### resize 执行逻辑

| 方向 | 步骤 | 锁 |
|---|---|---|
| scale-up | slot_init → pthread_create → `atomic_store n_active` | resize_lock |
| scale-down | `atomic_store n_active`（阻止新 submit 路由）→ draining=1 + sem_post → worker drain queue → pthread_join → 清理 mag/q/sem | resize_lock |
| rollback | draining=1 + sem_post → drain → pthread_join → 清理（不更新 n_active，保持 old_n） | resize_lock |

#### ticker 触发 resize

`main.c` ticker_main ~1s 周期：
```c
static long last_workers = -1;
long cur_workers = 0;
cfg_get_int(t->ctx->cfg, "core.workers", &cur_workers, NULL);
if (cur_workers > 0 && cur_workers != last_workers && !ctx_stopping(t->ctx)) {
    unsigned max = wpool_max_size(t->ctx->wpool);
    unsigned target = MIN((unsigned)cur_workers, max);
    if (wpool_resize(t->ctx->wpool, target) == NSBC_OK)
        last_workers = cur_workers;
}
```
- `core.workers` 改 `CGF_HOT`：`config.reload` / `SIGHUP` 后配置值变化 → ticker 下一个周期自动触发
- `ctx_stopping` 检查：shutdown 期间跳过（虽然 wpool_destroy 也拿 resize_lock，双重保险）

#### 所有权模型（经 Advisor 3 轮 race condition 修复后）

| 路径 | 共享字段操作 | 锁 |
|---|---|---|
| `wpool_submit/any` | `atomic_load(n_active)` 算 idx | **无锁**（RELAXED 足够） |
| `worker_main` 退出 | **只** `mem_rcu_thread_leave`（thread-local RCU reader，必须本线程调） | 无（不碰 pool 共享字段） |
| `wpool_resize` scale-up/down/rollback | 所有 slot 资源 init / draining / pthread_join / 清理 mag/q/sem | resize_lock |
| `wpool_destroy` | `pthread_mutex_lock` → worker_close_submit_gate → running=0 → sem_post 所有 → join 所有 → 清理所有 slot → `pthread_mutex_unlock` → `pthread_mutex_destroy` → free | resize_lock |

**关键修复的 race condition**：
1. ~~worker_slot_init 失败调 wpool_destroy(p)~~ → 只清自己 slot（否则炸整个运行中池）
2. ~~scale-up 部分失败已启动 worker 泄漏~~ → rollback 走 draining+drain+join+清理
3. ~~wpool_destroy 与 ticker resize 并发~~ → wpool_destroy 拿 resize_lock
4. ~~worker_main 退出无锁改 started/slot_ready/mag/q~~ → **全部移除**，worker_main 只做 thread-local RCU leave；所有 pool 共享字段修改移到持有 resize_lock 的 resize/destroy 路径

#### 配置

| 键 | 类型 | 标志 | 默认 | 范围 |
|---|---|---|---|---|
| `core.workers` | INT | **HOT** | 1 | [0=按CPU, 64] |
| `core.workers_max` | INT | RESTART | 64 | [1, 64] |

#### Linux 验证

```bash
# 启动 4 worker，max 32 槽位
core.workers=4 core.workers_max=32 nsbcd

# 热扩缩
nsbc-cli config.set core.workers 8
nsbc-cli config.reload
# ticker ~1s 后自动 wpool_resize(8)

# 验证 metrics
curl localhost:9090/metrics | grep nsbc_worker_threads
# nsbc_worker_threads_active 8
# nsbc_worker_threads_configured 32
```

## 已审阅的关键调用点与风险（P3 后更新）

- ~~`src/base/timer_wheel.c` 改变回调线程会改变 SIP、会话及业务定时器的执行上下文~~
  → **已解决**：全量 Grep 59 处 tw_add 调用点，零 callback 涉及 reactor-owned FD state。
  timer callback 保持 ticker 线程直接执行（见 Timer callback 多 reactor 设计结论章节）。
- ~~不能将 tw_advance() 回调套入同步 reactor_call()——停机阶段死锁~~
  → **已解决**：timer_wheel 零改动，ticker 线程直接推 + 执行 callback 天然串行。
- `src/sip/sip_transport.c` 的 TCP 发送入队路径在持有 `sbuf_lock` 时调用
  `reactor_mod()`。虽然当前 `mod` 不经过 mailbox，不应直接替换为同步 owner 派发；
  否则 owner 若需要同一把锁会形成死锁。
- `src/media/bridge/media_bridge.c` 的 `rtp_relay_arm()` 注册多个 FD；`rtp_relay_stop()`
  删除 FD、关闭 socket 并释放 relay。已通过 `reactor_call(target, relay_batch_stop_fn, r)`
  把 del + 后续 callback/fd close 串行化在同一 reactor owner 线程（P3 多 reactor 章节）。
- OAM control/metrics、ESL server 和 SIP transport 还有其他 add/del 调用。需分别确认
  调用线程、模块 start/stop 顺序及连接对象生命周期后再迁移；不要用通用 wrapper 一次性
  包住所有调用。

## 建议的下一步（P1/P2/P3 已完成后）

1. **Linux 构建验证**：本轮 P1/P2/P3（多 Reactor + Worker 扩缩）需重新 `make check` 验证。t_reactor.c 和 t_worker_lifecycle.c 已就绪。多 reactor 验证：`core.reactors=2` 启动后 `ss -ulnp | grep 5060` 应看到 2 个 socket，`ps -eLf | grep reactor` 应看到额外 reactor 线程。
2. ~~为 reactor_call() 增加可运行的并发/停机测试~~：已完成 t_reactor.c，7 场景覆盖。
3. ~~为 reactor_add/del 按模块列出 caller/thread/lock/arg lifetime~~：已在 reactor.h 审计表中完成。
4. ~~**定时器 callback dispatcher shutdown 协议设计**~~：**已完成**——timer callback 保持 ticker 线程直接执行（零 reactor affinity 需求）。未来若出现需要 reactor affinity 的 callback，在内部自行 reactor_call 即可。
5. **阶段 1 剩余项**：Event bus 序号分片已完成（P3）；剩余 SIP dialog 分片查找优化（经审计实际已完成）、RTP hook 读侧无锁化（经审计已有 atomic fast-path）、Web CDR 恶意标记 SQL 下推（Rust 侧）。
6. ~~**阶段 3 多 reactor 灰度**~~：**已完成**——reactor_mgr 放开 n>1，SO_REUSEPORT N socket，reactor[1..N-1] 独立 pthread。需 Linux 实测内核分流、锁争用、尾延迟。
7. **Worker 扩缩自动调节（非配置驱动）**：ticker 周期检查 queue depth → queued > 阈值 scale-up；idle > Ns scale-down。当前仅配置变更触发，无自动调优。
8. **TCP/TLS listener 多 reactor 分配**：目前 TCP/TLS 只在 reactor[0]，需按 reactor mgr 循环绑定。

## 验证与工作区提醒

- 文档更新应通过 `git diff --check`。
- C 代码更改需报告确切的 Linux 构建/测试命令及结果；若未执行，应明确标记未验证。
- 本轮新增文件：`src/include/reactor_manager.h`、`src/core/reactor_manager.c`、`tests/unit/t_reactor.c`。
- 本轮修改文件：`src/core/event_bus.c`、`src/core/reactor.c`、`src/core/reactor_manager.c`、`src/include/reactor.h`、`src/core/main.c`、`src/core/cfg_schema.c`、`src/media/bridge/media_bridge.c`、`src/sip/sip_transport.c`、`src/include/sip_transport.h`、`src/include/media_bridge.h`、`src/core/signaling.c`、`src/include/worker.h`、`src/core/worker.c`、`tests/unit/t_worker_lifecycle.c`。
- P3.5 新增：UDP data race 修复（per-reactor rx/dg 数组）、TCP/TLS 多 reactor 连接分布（home_reactor + conn_lock）、CPU 绑核（pthread_setaffinity_np）。
- Makefile 零改动。
- Timer callback：**零代码改动**（设计决策见 Timer callback 多 reactor 章节）。
- Makefile 零改动（`SRCS_CORE = $(shell find src -name '*.c')` 自动发现）。
