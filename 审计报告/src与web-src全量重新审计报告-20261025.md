# `src/` 与 `web/src/` 全量重新审计报告

审计日期：2026-10-25  
审计对象：当前工作区 `src/`、`web/src/`（含暂存、未暂存及未跟踪的相关实现）  
审计依据：[性能与并发架构改进建议-20261025.md](./性能与并发架构改进建议-20261025.md) 及当前代码  
审计性质：静态代码审阅与可用构建/测试验证；不是目标 Linux 设备上的压力测试或性能认证

## 1. 执行摘要

本次清点到 `src/` 下 306 个文件（其中 304 个 C 源码/头文件）、`web/src/` 下 51 个 Rust 源文件。重点复核了本轮修改和其跨模块生命周期/调用路径，包括 Reactor manager、Reactor mailbox、Worker 动态扩缩、SIP transport、RTP relay、Event bus、Registrar、Web CDR SQL 查询、认证和外部 API。另以全目录符号检索核对相关调用点。

**结论：当前版本不建议启用 `core.reactors > 1` 或 Worker 动态缩容。** 发现多处可能导致 use-after-free、数据竞争或停机期间释放仍被引用对象的问题；外部 API 的 `readwrite` key 还能通过原始命令代理访问超出该 scope 的命令。Web 正式二进制在本机可构建，但测试目标无法编译，因此新增/既有测试目前不能作为通过证据。

本报告将“全量”用于当前两个目录及其调用面的审计范围；受本次改动规模影响，结论重点来自变更文件逐项审阅和相关跨文件调用链检查，**不声称对 357 个文件的每一行都进行了逐行形式化证明**。对未修改、且不在上述调用链中的模块，不把未验证内容写成已排除风险。

## 2. 发现汇总

| 编号 | 严重度 | 文件/位置 | 发现 | 影响 |
|---|---|---|---|---|
| F-01 | 高（P1） | [routes/ext.rs](../web/src/routes/ext.rs#L77) | `readwrite` scope 可调用无命令白名单的原始 CLI 代理 | 可能绕过 scope 设计执行管理命令 |
| F-02 | 高（P1） | [worker.c](../src/core/worker.c#L456) | Worker 缩容与正在提交的任务之间存在队列释放竞态 | 可能向已释放队列写入，造成崩溃/内存破坏 |
| F-03 | 高（P1） | [main.c](../src/core/main.c#L837)、[reactor_manager.c](../src/core/reactor_manager.c#L64) | stop 后工作 Reactor 在线程内立即自销毁，早于模块 cleanup 完成 | RTP relay 清理可能访问已释放 Reactor 或释放仍被 callback 使用的对象 |
| F-04 | 高（P1） | [sip_transport.c](../src/sip/sip_transport.c#L760) | Reactor 0 的 accept callback 直接修改目标 Reactor 的 owner-only callback 表 | 多 Reactor 下存在并发读写；debug owner 检查下连接不会注册 |
| F-05 | 高（P1） | [reactor_manager.c](../src/core/reactor_manager.c#L158) | 启动线程部分失败后没有回滚 manager 状态，main 仍继续运行 | 可能使用已销毁的 Reactor，最终重复 destroy |
| F-06 | 高（P1） | [event_bus.c](../src/core/event_bus.c#L32) | Per-core ring 的单写者假设不成立，且超 32 CPU 的 ring 被合并到 ring 0 | 并发事件写入可覆盖帧、破坏 ring 元数据 |
| F-07 | 中（P2） | [reactor.c](../src/core/reactor.c#L174)、[reactor_manager.c](../src/core/reactor_manager.c#L79) | 预创建线程归属为 main，但工作线程执行 wait/destroy；debug owner 检查阻断多 Reactor | Debug 构建无法正常运行多 Reactor |
| F-08 | 中（P2） | [db/mod.rs](../web/src/db/mod.rs#L5917) | IVR 菜单 tenant 条件把占位符变量名当 SQL 字面文本 | 带 `tenant_id` 的列表查询失败 |
| F-09 | 中（P2） | [event_bus.h](../src/include/event_bus.h#L34)、[event_bus.c](../src/core/event_bus.c#L83) | per-core 序号不再满足公开接口声明的“全局单调递增” | 不同 core 可生成相同序号，跨核排序/去重语义改变 |
| F-10 | 中（P2） | [coreclient.rs](../web/src/coreclient.rs#L2411)、[routes/mod.rs](../web/src/routes/mod.rs#L51)、[main.rs](../web/src/main.rs#L840) | `cargo test` 编译失败：MockCore 返回类型不匹配，测试构造缺少新字段 | Web 测试套件无法执行 |
| F-11 | 中（P2） | [media_bridge.c](../src/media/bridge/media_bridge.c#L2332) | RTP relay 忽略各次 `reactor_add()` 错误并仍标记 `armed` | 部分 FD 注册失败时仍报告成功，媒体转发可能静默失效 |

## 3. 详细发现

### F-01：`readwrite` API Key 可通过原始命令代理越权执行管理命令

`/ext/v1/cmd` 在 [routes/ext.rs](../web/src/routes/ext.rs#L77) 只要求 `Readwrite` scope，然后将请求体清理换行后直接传给 `st.core.exec_cmd()`；该路径没有命令白名单，也没有按命令重新检查 `Admin` scope。该代理向 core 的管理控制通道发送命令，而控制通道看到的是 Web sidecar 身份，不是原 API Key 的权限。

结果是：只持有 `readwrite` key 的调用方可尝试调用被 scope 文档归为高风险的 core 命令，绕开结构化端点的权限边界。代码注释将原始命令描述为“任意控制命令”，而 scope 定义又把管理操作留给 `admin`，两者冲突。

**建议：** 将原始命令接口限制为 `admin`，或改为严格命令 allowlist，并为每个命令映射最低 scope；对未知命令必须拒绝。补充测试验证 `readonly`、`readwrite` 无法执行 admin-only 命令。

### F-02：Worker 缩容与并发提交之间存在 use-after-free

缩容路径在 [worker.c](../src/core/worker.c#L456) 先降低 `n_active`，随后标记高位 worker draining、join 线程并销毁其队列（约 456–479 行）。提交路径 [worker.c](../src/core/worker.c#L554) 不持有 `resize_lock`：它可先读取旧 `n_active` 并算出高位队列索引，然后在缩容线程 join 并释放 `w->q` 后才调用 `mem_mpsc_push()`（约 563–565 行）。`submit_state` 只用于 shutdown gate，不会让 resize 等待已进入的 submitter。

因此缩容所依赖的“先降低 `n_active` 就阻止新任务路由到被移除 worker”不成立：已读到旧值的提交仍可继续使用旧队列指针，可能写入已释放对象。`wpool_submit_any()` 有相同的窗口。

**建议：** 为 resize 与提交路由/入队建立可证明的同步协议：例如在关闭 worker 槽位前等待所有可能持有该槽索引的 submitter 离开，或采用稳定队列生命周期/引用计数；不得仅依赖原子 `n_active`。新增压力测试覆盖缩容与多提交线程并发，最好以 TSAN/ASAN 或等效工具验证。

### F-03：Reactor 线程可能在模块停止期间先销毁自身

主线程在 [main.c](../src/core/main.c#L837) 调用 `reactor_mgr_stop_all()`，随后才调用 `modmgr_stop_all()`（约 843 行）。工作 Reactor 收到 stop 后会退出循环、执行一次 drain，然后在线程入口中立即 `reactor_destroy()`，见 [reactor_manager.c](../src/core/reactor_manager.c#L64) 与 74–85 行。`reactor_mgr_join()` 虽然位于模块 stop 之后，但 join 只能等待线程，并不能延迟该线程已自行执行的 destroy。

模块停止阶段的 `rtp_relay_stop()` 会通过 [media_bridge.c](../src/media/bridge/media_bridge.c#L2417) 对 `home_reactor` 调用 `reactor_call()`。此时可能发生两种情况：

1. worker Reactor 已 destroy，调用方访问悬空指针；
2. Reactor 正在退出，`reactor_call()` 返回 shutdown/错误，但 relay cleanup 仍继续关闭 FD 并释放 relay，而目标 Reactor 的 callback 表尚未可靠摘除；若 callback 已在执行，存在并发释放风险。

**建议：** 分离“请求停止/退出 event loop”和“最终销毁 Reactor”两个阶段。先停止接受业务，再由 main 等待模块 cleanup、摘除所有 owner callback 并确认回调静止，之后 join，最后由受控的销毁路径释放所有 Reactor。停止期间的 relay 清理必须有确定的 owner-side drain/ack，不可失败后继续释放仍可能被引用的对象。

### F-04：TCP accept 路径跨线程直接写目标 Reactor callback 表

TCP/TLS listener 注册在 `ctx.re`（Reactor 0）；`on_tcp_accept()` 在该 Reactor 的 callback 中调用 `conn_accept()`。`conn_accept()` 按 peer 选择 `c->home_reactor`，随后在 [sip_transport.c](../src/sip/sip_transport.c#L760) 直接对该 Reactor 调用 `reactor_add()`。

当目标是 Reactor 1..N 时，调用发生在 Reactor 0 线程，而目标 Reactor 的 event loop 正在读取自己的 `slots[]`。`reactor_add()` 可能对该数组执行 `realloc`/写入，既违反 owner-only 约束，也与目标 Reactor 并发读写。启用 `NSBC_REACTOR_OWNER_CHECK` 时该 add 返回 `NSBC_PERM_DENIED`，而返回值未检查，连接仍会被当作已接受但没有事件注册。

**建议：** 在目标 Reactor owner 上通过有界 mailbox 完成 `reactor_add()`，检查返回码；add 失败时必须完整关闭并从连接索引/数组回滚。补充 TCP/TLS 多 Reactor accept、连接关闭和重连测试。

### F-05：Reactor 线程启动失败后的状态与资源回滚不一致

`reactor_mgr_start_threads()` 在部分线程已创建后发生参数分配或 `pthread_create()` 失败，会 stop/join 已创建线程；这些线程会在入口中自 destroy 对应 Reactor。但 manager 的 `started` 仍为 0，`m->n` 也不变。随后 [main.c](../src/core/main.c#L806) 只打印“continuing with n=1”并继续运行，实际 manager 仍保留原来的 N 个槽位；模块已在所有 Reactor 上完成 FD 注册，部分 Reactor 现在既没有线程也可能已释放。

之后 `reactor_mgr_destroy()` 的未启动分支会把所有 `m->reactors[i]` 再次 destroy；对已被工作线程释放的指针可能造成重复释放/未定义行为。

**建议：** 启动失败必须全量 rollback 并回到真正可工作的单 Reactor 状态，或直接 fail-fast 进入统一 shutdown。记录每个 Reactor 的实际创建、线程启动和销毁状态；不得以日志声称回退到 n=1 但保留 N 配置、注册和指针。

### F-06：Event bus ring 的“每 core 单写者”假设没有得到保证

`nsbc_event_push()` 通过 `sched_getcpu()` 选择 ring，然后以普通 load/copy/store 更新该 ring 的 `write_pos`；写路径没有 CAS 或 writer lock。`pcpt_idx()` 返回当前 CPU，但不固定线程，也不禁止抢占。多个 Reactor/Worker 线程可能在同一逻辑 CPU 上被调度；而 [event_bus.c](../src/core/event_bus.c#L32) 将 ring 数上限设为 32，CPU id 超出已分配 ring 数时又在约 131 行全部映射到 ring 0。因此在超过 32 个在线 CPU 的主机上，不同 CPU 上的并行 producer 会确定性地共享 ring 0。

并发 producer 可能读到相同 `write_pos`，写入重叠区域并发布相同的新位置，造成事件帧丢失或 ring 内容损坏。当前“per-core 单 producer”注释不是同步保证。

**建议：** 采用真正支持多 producer 的 per-core MPSC 写入协议，或在建立 ring 时覆盖所有可能 CPU 并为每个 ring 保证唯一 producer/固定 affinity。不能把超出上限的 CPU 静默折叠到 ring 0。加入多线程高并发、CPU affinity 与超过 32 CPU 的 ring 一致性测试。

### F-07：Debug owner 检查与 manager 的线程归属模型冲突

`reactor_create()` 将 owner 设为创建线程（当前是 main），见 [reactor.c](../src/core/reactor.c#L174)。manager 却让 Reactor 1..N 在线程入口调用 `reactor_wait()`，退出时也在工作线程调用 `reactor_destroy()`。开启 `NSBC_REACTOR_OWNER_CHECK` 时，`reactor_wait()` 和 `reactor_destroy()` 均会因为线程不等于 main owner 而返回 `NSBC_PERM_DENIED`；工作线程随后退出，manager 不能得到一个正常运行/销毁的多 Reactor 实例。

**建议：** owner 必须在线程真正绑定时建立，并保证所有创建、注册、wait、销毁操作遵循同一个 owner 规则；不要通过关闭 debug 检查来掩盖线程归属问题。

### F-08：IVR 菜单 tenant 查询使用无效 SQL 条件

在 [db/mod.rs](../web/src/db/mod.rs#L5917) 的 `list_ivr_menus()` 中，代码构造 `ph = "$N"` 后使用普通字符串 `where_parts.push("tenant_id = {ph}")`，不是格式化字符串。SQL 最终包含字面文本 `{ph}`，而不是占位符；`tenant_id` 非空时 count 查询会被 `unwrap_or(0)` 静默转成 0，列表查询则会报 SQL 错误。Rust 编译器也对此发出 `ph` 未使用警告。

**建议：** 使用 `format!("tenant_id = {ph}")`，或全程使用 `?` 占位符并按 backend 统一转换；count 查询错误应向调用方显式返回，避免把数据库错误伪装成空结果。

### F-09：per-core sequence 与公开全局序号契约不一致

`event_bus_next_seq()` 现在对每个 ring 独立从 1 递增（[event_bus.c](../src/core/event_bus.c#L83)），但公开结构和函数注释仍声明“全局单调递增”（[event_bus.h](../src/include/event_bus.h#L34)、约 65 行）。两个 core 会产生相同序号，序号不再能唯一标识跨 core 的事件，也不能用于全局排序。

**建议：** 若只要求 ring 内排序，明确修改公开 API/文档并确保所有 handler/外部消费者不把序号当全局唯一值；若保留全局契约，则采用分段号/唯一编码等方案，并测试跨 core 唯一性及单调语义。

### F-10：Web 测试目标不能编译

本机执行 `cargo test --locked --offline` 时，测试目标在编译阶段失败，尚未运行测试。错误包括：

- [coreclient.rs](../web/src/coreclient.rs#L2411) 的 MockCore `security_ban_list` / `security_ban_unban`：`unwrap_or_else()` 分支返回 `String`，但被操作的值是 `Result<String, CoreError>`，后续解析又把 `Result` 当 `&str` 使用。
- [routes/mod.rs](../web/src/routes/mod.rs#L51) 与 [main.rs](../web/src/main.rs#L840) 中测试用 `AppState` 初始化遗漏新字段 `require_loopback_auth`。

`cargo build --locked --offline` 通过，但有 39 条 warning，其中 `list_ivr_menus()` 的无效 tenant 占位符也产生了未使用变量警告。生产 build 成功不能替代测试目标通过。

**建议：** 先修复 MockCore 的 `Result` 默认值类型与所有测试 `AppState` 初始化，再运行整个 Web 测试目标；将该命令纳入 CI。

### F-11：RTP relay 注册失败被当作成功

`relay_batch_arm_fn()` 连续调用最多四次 `reactor_add()`，不检查任何返回值，随后无条件设置 `r->armed = 1` 并返回 `NSBC_OK`（[media_bridge.c](../src/media/bridge/media_bridge.c#L2332)）。单个 FD 注册失败时，其余 socket 可能仍工作，但调用者会认为 relay 已全部 armed；之后 stop 也可能因未注册 FD 的 `reactor_del()` 失败而留下不一致状态。

**建议：** 检查每个 add 的返回值；部分失败时在 owner 线程回滚已经成功的注册并返回明确错误，不能设置成功状态。

## 4. 对原改进建议的复核

- **Registrar：** AoR 索引和联系人节点复用可减少更新路径扫描/分配；当前新增单测覆盖了联系人上限替换。Linux 专项测试、锁等待/分片分布与吞吐基准仍未提供。
- **SIP transaction：** 分片内哈希桶和复用哈希路径已经存在；当前没有事务专项测试或性能基准，无法证明重传、响应、ACK/CANCEL、超时行为与优化收益。
- **SIP dialog：** 已使用分片 bucket；本次没有完成其并发删除与引用保护的完整压力验证。
- **RTP：** Reactor owner 路由意图已实现，但 F-03、F-04、F-11 表明现有启动、关闭和失败回滚路径仍不安全。
- **Event bus：** 全局序号争用被移除，但 F-06 的 ring 多 producer 风险及 F-09 的全局序号契约变更尚未解决。
- **Web CDR：** 恶意标记过滤/计数/分页已下推 SQL，消除了应用层最多扫描 100,000 行导致的漏项。但 `metadata` JSON 条件和 `from_ip` 子串过滤仍可能扫描大量/全部 CDR；普通 B-tree 无法有效支持 `%substring%`。需要目标后端的索引/查询计划与大表性能测量，不能仅凭下推 SQL 宣称已解决查询扩展性。
- **Worker：** semaphore 等待、关闭 gate 和指标注销已加入；F-02 说明动态缩容仍有提交竞态，当前不应启用热扩缩。
- **多 Reactor：** 默认单 Reactor 可作为现有模式；但线程启动失败、owner 归属、TCP accept 到目标 Reactor 的注册和 shutdown 生命周期未闭环。建议暂时保持 `core.reactors=1`，修复并完成端到端协议测试后再启用 2/4 Reactor。

## 5. 验证记录与限制

| 检查 | 结果 |
|---|---|
| `cargo build --locked --offline`（`web/`） | 通过；出现 39 条 warning |
| `cargo test --locked --offline`（`web/`） | 失败：测试目标编译错误，测试未运行 |
| `git diff --check` 与 `git diff --cached --check` | 通过 |
| C 单元测试（`tests/unit/`） | 未运行：当前 Windows 环境无可用 WSL Linux distribution / 本机 `make` 工具 |
| Linux 多 Reactor、RTP/SIP 集成、ASAN/TSAN 压测 | 未运行 |
| 性能基准（CPS、RTP p99、CDR 查询计划与延迟） | 未提供；本报告不声称性能已提升 |

## 6. 建议处理顺序

1. 先关闭或限制外部原始命令代理的 scope（F-01）。
2. 修复 Worker resize 与提交同步（F-02）。
3. 重做 Reactor 停机/销毁顺序及部分启动失败回滚（F-03、F-05）。
4. 把 TCP 新连接注册通过 owner mailbox 路由，并处理所有 add/del 错误（F-04、F-11）。
5. 修复 Event bus 的多 producer 安全和 sequence 契约（F-06、F-09）。
6. 修复 IVR tenant SQL 与 Web 测试编译问题（F-08、F-10）。
7. 在 Linux 上运行全部新增 C 测试、协议回归和多轮压测；未通过前维持单 Reactor，且不宣称性能收益。
