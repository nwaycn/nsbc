# `src/` 与 `web/src/` 全量复审报告

审计日期：2026-10-25  
审计对象：当前工作区 `src/` 与 `web/src/`  
审计方式：两条独立源码审阅（C 核心、Rust Web），结合当前工作区的 Rust 构建/测试结果与 diff 检查  
性质：静态代码复审；不替代 Linux 目标环境集成测试、运行时安全验证或性能压测

## 1. 结论

复审覆盖了 `src/` 下 306 个文件（304 个 C/H 文件）和 `web/src/` 下 51 个 Rust 文件的清点，并对 Reactor/Worker、SIP transport、RTP、Event bus、CDR 持久化、认证/RBAC、Web API、数据库查询及应用启动等高风险路径进行了重点审阅和跨文件调用点核对。

当前状态存在多项需优先处理的问题：

- 多 Reactor 模式有连接注册、停机销毁、线程启动失败回滚等生命周期问题；**不建议将 `core.reactors` 配置为大于 1**。
- Worker 缩容与并发任务提交存在竞态；**不建议启用运行时缩容**。
- Event bus 的事件投递在当前集成中未初始化/未消费，同时其 ring 写侧也不能保证单写者安全。
- Web 管理面存在默认已知管理员密码、RBAC 权限不足、TLS 初始化失败后降级为明文 HTTP 等高风险问题。
- CDR spool 在连续持久化失败时会覆盖先前批次，可能造成离线 CDR 丢失。
- Web 主程序可编译，但 `cargo test` 测试目标编译失败，当前没有有效的 Rust 测试通过证据。

本次为全面的风险导向复审，覆盖目录清点、改动重点和高风险调用链；**不是对 357 个文件逐行作形式化证明**。未逐项核验的低风险、未修改模块和运行时行为均不视为已排除风险。

## 2. 发现总表

| ID | 严重度 | 位置 | 摘要 |
|---|---|---|---|
| C-01 | 高 | [worker.c](../src/core/worker.c#L456) | 缩容与并发提交竞态，可能丢任务或访问已释放队列 |
| C-02 | 高 | [main.c](../src/core/main.c#L837)、[reactor_manager.c](../src/core/reactor_manager.c#L64) | secondary Reactor 在线程退出时自销毁早于模块 cleanup |
| C-03 | 高 | [sip_transport.c](../src/sip/sip_transport.c#L658) | TCP/TLS accept 线程直接向另一 Reactor 注册 fd |
| C-04 | 高 | [reactor_manager.c](../src/core/reactor_manager.c#L158) | 部分 Reactor 线程启动失败后 manager 留有已销毁对象 |
| C-05 | 高 | [event_bus.c](../src/core/event_bus.c#L48) | 当前没有初始化/消费 Event bus ring，异步事件不能正常投递 |
| C-06 | 高 | [event_bus.c](../src/core/event_bus.c#L32) | ring producer 不保证单写者，超过 32 CPU 时多个 CPU 合并到 ring 0 |
| C-07 | 高 | [cdr.c](../src/mod/billing/cdr.c#L147) | CDR spool 用覆盖写；连续失败时丢失此前 spool 批次 |
| W-01 | 高 | [main.rs](../web/src/main.rs#L520)、[mod.rs](../web/src/db/mod.rs#L323) | 未配置管理员密码时初始化已知默认密码 |
| W-02 | 高 | [rbac.rs](../web/src/middleware/rbac.rs#L59)、[calls.rs](../web/src/routes/calls.rs#L50) | 一些标为 L2 的呼叫控制端点实际只要求 L1 |
| W-03 | 高 | [ext.rs](../web/src/routes/ext.rs#L77) | readwrite API Key 可提交任意 core 命令 |
| W-04 | 高 | [main.rs](../web/src/main.rs#L471) | 配置了 TLS 但初始化失败时继续以明文 HTTP 提供服务 |
| W-05 | 中 | [mod.rs](../web/src/db/mod.rs#L217) | MySQL 后端的部分认证 SQL 未经过占位符转换 |
| W-06 | 中 | [mod.rs](../web/src/db/mod.rs#L5917) | IVR 菜单 tenant 条件构造为字面量 `{ph}`，查询错误/结果异常 |
| W-07 | 中 | [auth.rs](../web/src/routes/auth.rs#L351)、[sessions.rs](../web/src/auth/sessions.rs#L50) | 密码修改/账户状态或角色变更后旧 session 仍可继续刷新 |
| C-08 | 中 | [reactor.c](../src/core/reactor.c#L174)、[reactor_manager.c](../src/core/reactor_manager.c#L79) | debug owner check 与 secondary Reactor 线程归属冲突 |
| C-09 | 中 | [media_bridge.c](../src/media/bridge/media_bridge.c#L2332) | RTP relay 忽略 `reactor_add()` 错误并无条件报告 armed |
| C-10 | 中 | [event_bus.h](../src/include/event_bus.h#L34)、[event_bus.c](../src/core/event_bus.c#L83) | per-core 序号实现与公开“全局单调序号”契约冲突 |
| W-08 | 中 | [coreclient.rs](../web/src/coreclient.rs#L2411)、[routes/mod.rs](../web/src/routes/mod.rs#L51) | `cargo test` 测试目标编译失败 |

## 3. C 核心详细发现

### C-01：Worker 缩容与任务提交存在竞态

缩容在 [worker.c](../src/core/worker.c#L456) 先更新 `n_active`，随后让被移除 worker drain、join 并销毁队列。提交者在 [worker.c](../src/core/worker.c#L554) 不持 `resize_lock`，会独立读取 `n_active`，再选队列并入队。

已经读取旧 `n_active` 的提交者可能在被移除 worker 观察到队列为空后才 enqueue。这样任务可能在 worker 退出后仍留在队列，或与队列销毁并发；提交 API 还可能已经返回成功。`wpool_submit_any()` 也走类似无同步路径。

**建议：** 缩容必须等待所有可能持有待移除槽位索引的提交者完成，或用引用/epoch 等稳定生命周期协议。增加多提交线程与 resize 并发测试，在 Linux 上用 TSAN/ASAN 或等效工具验证。

### C-02：secondary Reactor 可在模块 cleanup 前销毁

主流程在 [main.c](../src/core/main.c#L837) 先 stop 所有 Reactor，之后才执行 `modmgr_stop_all()`。secondary Reactor 线程在 stop 后退出并在 [reactor_manager.c](../src/core/reactor_manager.c#L79) 自行 `reactor_destroy()`。模块停止路径仍可能从 [media_bridge.c](../src/media/bridge/media_bridge.c#L2417) 使用 `home_reactor` 调 `reactor_call()`。

**影响：** 当 relay 归属 secondary Reactor 时，cleanup 可能访问已释放的 Reactor；或者 mailbox 已关闭后 `reactor_call()` 失败，relay 仍继续释放其 FD/状态，而 owner callback 的完成/摘除未得到确认。

**建议：** stop、模块 cleanup、事件静止确认、join 和 destroy 必须分成明确阶段。Reactor 的最终释放由 manager/main 在模块 cleanup 与所有 owner-side callback drain 完成后执行。

### C-03：TCP/TLS accept callback 跨 owner 线程注册连接 fd

TCP/TLS listener 目前由 Reactor 0 接收连接；`conn_accept()` 按 peer hash 指定 `home_reactor`，但随后从 accept callback 所在线程直接调用 `reactor_add(c->home_reactor, ...)`（[sip_transport.c](../src/sip/sip_transport.c#L658)、[sip_transport.c](../src/sip/sip_transport.c#L760)）。

当目标为另一个 Reactor 时，其 event loop 正在读取自身 callback slot 表；跨线程 `reactor_add()` 可能造成并发读写或 slot `realloc` 竞态。该调用没有检查返回值，debug owner check 下连接注册失败后仍可能被当作成功连接。

**建议：** 通过目标 Reactor 的 owner mailbox 同步注册；失败时从连接数组/索引回滚、关闭连接并上报可观测错误。增加多 Reactor TCP/TLS accept、读写、关闭和重连测试。

### C-04：部分启动失败后 manager 状态与对象生命周期不一致

`reactor_mgr_start_threads()` 在部分 `pthread_create()` 失败时 stop/join 已启动线程；线程退出时会自销毁自己的 Reactor，但 manager 仅在所有创建成功后才设置 `started`。main 随后只记录“继续以 n=1”，没有重置 manager 的 Reactor 数量、已注册 socket 或指针。

后续 manager 操作可能使用已释放 Reactor，`reactor_mgr_destroy()` 也可能按 `started == 0` 再次销毁旧指针。

**建议：** 部分启动失败必须可靠回滚到真正可工作的单 Reactor 状态，或 fail-fast 走统一清理。为每个 Reactor 单独记录 thread started / object alive 状态，确保 destroy 唯一且错误被上层处理。

### C-05：Event bus 在当前代码集成中未初始化且没有消费方

在 `src/` 的调用关系中，`event_bus_init()` 只有定义，没有启动集成调用；`nsbc_event_push()` 在 ring 未初始化时返回 `NSBC_INVAL`。另外，当前源码没有推进 ring 的 `read_pos` 的消费者实现。因此即便补上初始化，ring 最终会填满并持续拒收事件。

**影响：** 注释中承诺的异步 CDR/监控事件通道当前不可用，push 的返回值若未检查会导致静默丢事件。

**建议：** 明确初始化、consumer 生命周期、读位置推进、关闭 drain 和错误处理；在事件实际投递的集成测试通过前，不将该总线描述为已交付的异步通道。

### C-06：Event bus ring 的单 producer 假设缺少同步保证

`nsbc_event_push()` 使用 `sched_getcpu()` 选择 ring，再对 `write_pos` 做普通读取、buffer copy 和 store；CPU 亲和性/单线程约束没有建立。线程迁移、多个 Reactor/Worker 同时运行在同一 CPU 都可能使同一 ring 有多个 producer。此外 ring 数量被限制为 32，超出 ring 数的 CPU 统一回退到 ring 0。

多个 producer 同时获取相同 `write_pos` 可覆盖帧并错误发布新位置。

**建议：** 改为真正的 MPSC ring 协议，或以运行时机制确保单 writer 且 ring 覆盖全部可能 CPU；不得静默合并多个 CPU 的 producer。加入并发 producer、CPU affinity 和高 CPU 数测试。

### C-07：CDR spool 新批次覆盖旧批次

`spool_write()` 以 `"wb"` 打开临时 spool 文件并替换正式 spool（[cdr.c](../src/mod/billing/cdr.c#L147)、[cdr.c](../src/mod/billing/cdr.c#L166)）。flush 路径会推进 `flush_seq`，然后尝试持久化/写 spool（[cdr.c](../src/mod/billing/cdr.c#L543)、[cdr.c](../src/mod/billing/cdr.c#L572)）。

数据库持续不可用且多个 flush 批次陆续落到 spool 时，后续成功写出的 spool 可能替换掉此前批次，留下最后一批而丢失更早的离线 CDR。

**建议：** 将 spool 改为可追加、分段文件或带确认游标的持久队列；只有已持久化的数据才推进清理游标。用连续多批次 DB 故障/恢复测试验证完整回放与顺序。

### C-08：Debug owner check 与 manager 的线程归属相冲突

Reactor 在 main 线程创建并记录该线程为 owner；manager 在 secondary pthread 中调用 `reactor_wait()`，退出时又在该线程调用 `reactor_destroy()`。开启 `NSBC_REACTOR_OWNER_CHECK` 时，这些调用会因线程归属不符而拒绝。

**建议：** owner 身份应在线程真正创建/绑定 Reactor 时确定，保证 debug 与生产构建执行同一线程归属模型；不能依赖关闭检查来使功能运行。

### C-09：RTP relay 对 fd 注册失败返回成功

`relay_batch_arm_fn()` 对多路 RTP/RTCP fd 调用 `reactor_add()` 后不检查各次结果，仍将 `armed` 置为 1 并返回 `NSBC_OK`（[media_bridge.c](../src/media/bridge/media_bridge.c#L2332)）。

**影响：** 部分 fd 注册失败时调用方无法察觉 relay 不完整；stop 也可能对从未注册的 fd 执行删除，造成状态不一致。

**建议：** 检查每个 add 结果，在 owner 线程回滚已成功注册的 fd，并只在全部注册成功后标记 armed。

### C-10：事件序号的实现不再符合全局序号声明

实现把序号拆为每 ring 独立递增，但头文件仍将 `event_seq` 和 `event_bus_next_seq()` 描述为全局单调递增。多个 core 可以产生相同序号，不能再用于全局去重或跨 core 排序。

**建议：** 要么修订公开契约并审计消费者，要么实现跨 ring 唯一且排序语义明确的序号；添加跨核测试，避免注释和代码语义不一致。

## 4. Rust Web 详细发现

### W-01：初始化管理员账号使用已知默认密码

当没有设置 `NSBC_DEFAULT_ADMIN_PW`，且配置未提供 `auth.default_admin_pw` 时，启动路径使用硬编码默认密码 `Admin123` 创建管理员账号（[main.rs](../web/src/main.rs#L520)、[db/mod.rs](../web/src/db/mod.rs#L323)）。登录从 `web_users` 表验证该账号（[auth.rs](../web/src/routes/auth.rs#L140)）。

**影响：** 新数据库部署若暴露在不可信网络，已知凭据可获得管理员权限。

**建议：** 生产环境无显式管理员密码时拒绝启动/拒绝默认账号登录；或强制一次性初始化流程生成随机凭据并要求首次更改。对外暴露场景不得使用固定默认密码。

### W-02：L1 身份可调用部分标记为 L2 的呼叫控制端点

呼叫控制路由将 kill、answer、transfer、record 等操作注释/标记为 L2（[calls.rs](../web/src/routes/calls.rs#L50)），但全局 `required_level()` 仅匹配有限路径；未显式匹配的写操作默认 L1（[rbac.rs](../web/src/middleware/rbac.rs#L59)）。

**影响：** L1 JWT 可调用原本设计为 L2 的呼叫控制操作。

**建议：** RBAC 应以完整 method + route 模板映射权限，未知写路径默认拒绝或显式 L1，而不能与端点自身的权限声明相矛盾。补齐每个 L0/L1/L2 route 的授权矩阵测试。

### W-03：readwrite API Key 可执行任意 core 命令

`/ext/v1/cmd` 只检查 `Readwrite` scope，再将调用者命令传入 `exec_cmd()`（[ext.rs](../web/src/routes/ext.rs#L77)）。该通道没有按 core 命令检查 Admin scope，也没有白名单。

**影响：** readwrite key 可能绕过结构化 API 的权限边界，访问配置、reload 或其他高权限命令。

**建议：** 原始命令接口只允许 admin scope，或使用严格 allowlist 和逐命令权限表；未知命令拒绝。增加 scope 越权回归测试。

### W-04：TLS 初始化失败时服务降级为明文 HTTP

当 TLS 被配置启用但 TLS 初始化失败时，启动代码记录错误并将 TLS source 置空，之后仍启动 HTTP listener（[main.rs](../web/src/main.rs#L471)）。

**影响：** 证书损坏、权限错误或 TLS 配置错误可导致登录密码和 bearer token 经明文传输，尤其是 bind 到非 loopback 地址时。

**建议：** `tls.enabled=true` 时 TLS 初始化失败应 fail closed，不应静默退化为明文。若确需 HTTP，应由独立显式配置决定并在启动日志/健康状态中清楚呈现。

### W-05：MySQL 后端认证查询可能使用错误占位符语法

DB 层定义了 `fix_ph()` 处理后端占位符差异，但 `get_web_user()`、密码更新和默认用户种子查询直接使用 `$1` 等 SQL（[db/mod.rs](../web/src/db/mod.rs#L217)、约 233、273 行）。相关 ext key/webhook 更新查询也存在未转换的 `$1` 用法。

**影响：** 这些查询在 MySQL 方言下可能失败，影响登录、改密、默认账户初始化及 key/webhook 管理。

**建议：** 将所有 `Any` 查询统一经过 backend placeholder helper，或统一采用 sqlx Any 支持的绑定语法；对 SQLite、PostgreSQL、MySQL 分别做集成测试。本问题需要目标 SQLx Any / MySQL 版本验证，因此列为中等级别并注明后端条件。

### W-06：IVR 菜单 tenant SQL 条件无效

`list_ivr_menus()` 将动态生成的占位符名放进普通字符串 `"tenant_id = {ph}"`，而非格式化字符串（[db/mod.rs](../web/src/db/mod.rs#L5917)）。tenant 条件启用时 SQL 会包含 `{ph}` 字面文本；count 查询还会把执行错误吞为 0。

**建议：** 正确拼接占位符并保留参数绑定；count 查询数据库错误应传播，不能伪装成零条数据。

### W-07：密码更改/账户变更后现有 session 仍可刷新

密码更改处理明确没有撤销现有 token；refresh 路径仅消费内存中的 refresh session，再按 session 内保存的 subject/role 签发新 token，不重新验证账户状态或当前角色（[auth.rs](../web/src/routes/auth.rs#L270)、约 351 行；[sessions.rs](../web/src/auth/sessions.rs#L50)）。

**影响：** 密码更改、用户禁用或角色降级后，之前的 refresh token 仍可能继续续期，并保留旧权限，直至 refresh session 被撤销或过期。

**建议：** 对密码修改、禁用、角色降级和管理员强制重置提供统一的 session/token version 或逐用户撤销机制；refresh 时校验用户当前 enabled/role。

### W-08：Rust 测试目标编译失败

本机执行 `cargo test --locked --offline` 未能编译测试目标：

- MockCore 的 `security_ban_list` / `security_ban_unban` 对 `Result<String, CoreError>` 使用了返回 `String` 的默认分支，后续又把 `Result` 当字符串处理（[coreclient.rs](../web/src/coreclient.rs#L2411)）。
- 测试构造的 `AppState` 缺少 `require_loopback_auth` 字段（[routes/mod.rs](../web/src/routes/mod.rs#L51)、[main.rs](../web/src/main.rs#L840)）。

**影响：** 本轮 Web 单元/路由测试无法执行，相关行为没有测试覆盖证据。

**建议：** 修复 MockCore 返回类型与测试状态初始化，之后在 CI 执行完整 `cargo test --locked`。

## 5. 条件性部署风险

### 反向代理下的 loopback API Key 免鉴权

loopback peer 在 `require_loopback_auth=false` 时被直接授予 admin claims，该配置默认 false（[ext_key.rs](../web/src/middleware/ext_key.rs#L90)、[config.rs](../web/src/config.rs#L57)）。若外部反向代理把远程请求转发成来自 loopback 的连接，且运维未启用 `web.ext.require_loopback_auth`，请求会绕过 API Key 校验。

配置注释已提示该部署要求；这是依赖网络拓扑的条件性风险。建议生产部署默认启用 loopback auth，或仅在能可信区分本地客户端时允许绕过。

## 6. 验证记录

| 验证项 | 结果 |
|---|---|
| 文件清点 | `src/` 306 个文件（304 个 C/H）；`web/src/` 51 个 Rust 文件 |
| `cargo build --locked --offline`（`web/`） | 通过；存在 39 条 warning |
| `cargo test --locked --offline`（`web/`） | 失败；测试目标编译错误，未运行测试 |
| `git diff --check` / `git diff --cached --check` | 通过 |
| C `make check` | 未运行：环境无 `make`/GCC，也没有可用 WSL Linux 发行版 |
| 多 Reactor、SIP/RTP 集成、TLS/DB 后端集成 | 未运行 |
| ASAN/TSAN、长时稳定性、性能基准 | 未运行 |

因此，本报告的并发、协议、数据库后端和网络边界结论均为源码审阅发现；需要按建议补充运行时测试，不能据此声称已经达到性能或发布验收门槛。

## 7. 优先整改顺序

1. Web 安全：移除已知默认管理员密码；修复 L2 RBAC 漏洞、原始命令 scope 越权和 TLS fail-open。
2. CDR 持久化：修正 spool 多批次覆盖与恢复确认语义。
3. Worker：为 resize 与 submit 建立同步/生命周期协议。
4. 多 Reactor：修复 cross-owner fd 注册、stop/cleanup/destroy 顺序与部分启动失败回滚；修复前保持 `core.reactors=1`。
5. Event bus：接入初始化/消费生命周期，建立多 producer 安全，并统一序号契约。
6. 数据库兼容性：修复 IVR tenant SQL 和 MySQL placeholder 转换，测试各后端。
7. Session 生命周期：确保密码与账户权限变更能使旧 session 失效。
8. 修复 Rust 测试目标并补齐 C 并发/协议专项测试，在 Linux CI 和目标设备上执行。
