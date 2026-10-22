# NSBC Event/CDR 实时订阅客户端（多语言）

本目录包含 NSBC control socket（Unix Domain Socket）实时事件/CDR 订阅客户端的 **四种语言实现**，功能完全等价：

| 语言 | 目录 | 依赖 | 编译/运行 |
|---|---|---|---|
| **Python** | `python/` | 仅 stdlib | `python3 event_client.py` |
| **Go** | `go/` | 仅 stdlib | `go run main.go` 或 `go build -o event_client .` |
| **Rust** | `rust/` | 仅 stdlib | `cargo run --release --` |
| **C** | `c/` | 仅 POSIX | `gcc -O2 -Wall -o event_client event_client.c && ./event_client` |

默认连接 `/tmp/nsbc-e2e/control.sock`，用 `--topics` 或位置参数覆盖。

---

## 协议（服务端帧格式）

nsbcd 经 `obs_sub` 推送的所有帧都是 **以 `\n` 结尾的单行文本**，客户端按 `readline` 收即可。

### Events 帧（`sub events`）

```
YYYY-MM-DDTHH:MM:SS.mmm CALL_INCOMING  call_id=... orig=1001 to=1002 src_ip=10.0.0.50 trunk=
YYYY-MM-DDTHH:MM:SS.mmm CALL_ANSWERED  call_id=... orig=1001 to=1002 trunk=
YYYY-MM-DDTHH:MM:SS.mmm CALL_END       call_id=... orig=1001 to=1002 trunk= disp=ANSWERED cause=200 duration_s=120 billsec=116
YYYY-MM-DDTHH:MM:SS.mmm REGISTER       ...
```

格式: `ISO8601时间戳 域(DOMAIN) k=v k=v ...\n`

### CDR 帧（`sub cdr`）

```
YYYY-MM-DDTHH:MM:SS.mmm CDR {"call_id":"...","from":"1001","to":"1002","phase":"end",
                             "disposition":"ANSWERED","duration_s":120,"billsec":116,...}
```

格式: `ISO8601时间戳 CDR <JSON>\n`

JSON 契约与 Web 侧 REST 返回完全对齐（见 [signaling.c](../../../src/core/signaling.c#L892) 的 `cdr_event_to_json`）。

### Log / Metrics 帧（`sub log` / `sub metrics`）

- `log`: 直接透传 log ring buffer 的日志行
- `metrics`: Prometheus exposition 周期帧（默认 5s）

### 命令协议

客户端连上后，nsbcd 先发 banner + 提示符：

```
NSBC 2.6.0 control interface (try 'help')
nsbc> 
```

命令以 `\n` 结尾，提示符 `nsbc> ` 表示命令执行完毕。

实时订阅命令：

| 命令 | 说明 |
|---|---|
| `sub <topics>` | 订阅 topics（逗号分隔：events,cdr,log,metrics） |
| `unsub [topics]` | 取消订阅（无参取消全部） |
| `sub.stat` | 查看本连接订阅状态 |

常用拉取命令（`pull-only` 模式下可用）：

| 命令 | 说明 |
|---|---|
| `events [N]` | 拉最近 N 条 events（默认 20） |
| `show calls` | 活动呼叫表 |
| `cdr.recent [N]` | 拉最近 N 条已完成 CDR（默认 500） |
| `show cdr` | CDR sink 聚合计数器 |

---

## 性能设计（服务端）

| 项 | 策略 |
|---|---|
| **呼叫线程零反压** | `obs_sub_publish` 锁内 memcpy 帧入订阅者独立帧环（每订阅者 `OBS_SUB_RING=16`），满丢最旧帧，**绝不阻塞** |
| **慢消费者** | 每连接独立帧环；`obs_sub_publish` 立刻 `try flush`（非阻塞 `send`，`EAGAIN` 归帧环） |
| **帧截断** | 单帧 `OBS_FRAME_MAX=2048` 字节截断，避免超大 JSON 撑爆 |
| **订阅上限** | `OBS_SUBSCRIBER_MAX=256` 个并发订阅者 |
| **拉取式** | `obs_event` 同时写环形缓冲 `OBS_EVENT_RING=1024`，支持 `events N` 拉取 |

### 锁序（无死锁）

```
call_lock (b2bua shard)
  → obs_event: g_ev.lock → obs_sub_hub.lock
s->lock (cdr sink) —先 unlock 再 publish—
  → obs_sub_hub.lock
```

send_fn 回调只做 `send()`，不递归进 b2b/cdr 代码——锁序单向，安全。

---

## 运行步骤

```bash
# 1. 起 nsbcd
nsbcd -c /tmp/nsbc-e2e/nsbc.yaml

# 2. 任选一种客户端（同机或跨机用 TCP 转发 UDS 即可）
cd examples/event_client/python   && python3 event_client.py
cd examples/event_client/go       && go run main.go
cd examples/event_client/rust     && cargo run --release --
cd examples/event_client/c        && gcc -O2 -o event_client event_client.c && ./event_client

# 3. 软电话注册到 nsbcd 并拨打电话
#    alice (10.0.0.50) → 拨 bob
#    客户端应看到 CALL_INCOMING → CALL_ANSWERED → CDR → CALL_END
```

---

## 与 FreeSWITCH event_socket 对比

| 项 | FreeSWITCH | NSBC |
|---|---|---|
| 传输 | TCP（可选 TLS） | **Unix Domain Socket**（P0）；TCP 后续 |
| 鉴权 | `auth <password>` | 文件权限 0600（root-only socket）；TCP 后续加令牌 |
| 订阅命令 | `event plain ALL` | `sub events,cdr` |
| 推送帧格式 | `Content-Type: ... \n\nBODY` | 单行 `YYYY..SS.mmm DOMAIN body\n` |
| 慢消费者策略 | 反压（阻塞呼叫线程） | **帧环丢旧，绝不反压** |
| 事件域 | CHANNEL_CREATE/BEGIN/ANSWER/HANGUP/HANGUP_COMPLETE + 200+ | **最小集** CALL_INCOMING/ANSWERED/END + REGISTER |
| CDR | 单独 CDR socket 或 `cdr_csv` | **同通道** `sub cdr`，带 ISO8601 时间戳头 |

NSBC 刻意保持最小事件集——实际运营场景 99% 只需要呼叫生命周期三件事 + CDR 最终话单。

