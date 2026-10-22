# NSBC 模块示例与启用指南

本目录存放 NSBC SIP B2BUA 各模块的编写模板、启用示例和说明。

---

## 目录

| 文件 | 说明 |
|------|------|
| [module_template.c](module_template.c) | **新模块编写起步模板**（含 3 种写法示例和完整注释） |
| [01_enable_builtins.yaml](01_enable_builtins.yaml) | 内建已注册模块的 YAML 配置模板 |
| [02_enable_new_mods.cdiff](02_enable_new_mods.cdiff) | 为 src/mod/ 下"写了但未注册"的模块启用所需的代码 diff |
| [03_makefile_srcs_hack.md](03_makefile_srcs_hack.md) | Makefile SRCS 白名单追加说明 |

---

## 快速上手：三步加一个新模块

### 1. 写 .c 文件

复制模板到 `src/mod/myfeature/myfeature.c`，按模板注释改。核心是结尾的一行宏：

```c
NSBC_MOD0(myfeature, 1, MOD_L_FINAL, MOD_F_REQUIRED, myf_init, myf_fini);
```

- `NSBC_MOD0` = 无依赖、无 start/stop 的极简写法；需要 start/stop 或 deps 时用 `NSBC_MOD`
- `level` 取值见 `src/include/module.h` 的 `mod_level_t`，一般选 `MOD_L_FINAL`
- `flags` 按需 OR：`MOD_F_CALL_PATH`（呼叫面模块）、`MOD_F_REQUIRED`（失败即启动失败）

### 2. 注册到 builtin.c

打开 `src/core/builtin.c`，加两行：

```c
extern const nsbc_module_t nsbcm_myfeature;   // 加在现有 extern 列表里

// 在 nsbc_builtin_modules[] 数组里追加：
&nsbcm_myfeature,
```

表以 `(const nsbc_module_t *)0` 结尾，顺序不重要（模块管理器内部做拓扑排序）。

### 3. Makefile 加一行

打开根目录 `Makefile`，找到第 24 行的 `SRCS := $(shell (find ...`，在现有白名单目录后面加一个：

```makefile
find src/mod/myfeature -name '*.c';
```

然后 `make && ./nsbcd -T` 验证配置能过。

---

## 模块分层与依赖顺序

参考 `src/include/module.h` 的 `mod_level_t`：

```
MOD_L_EARLY          ← 仅自检/注册（例：dbal）
  ↓
MOD_L_MEM_READY      ← 内存五域/RCU 就绪
  ↓
MOD_L_NET_READY      ← reactor / socket 就绪
  ↓
MOD_L_WORKER_READY   ← worker 池/时间轮就绪（例：endpoints, outreg, dialplan）
  ↓
MOD_L_FINAL          ← 接收流量前最后一层（例：signaling, billing, recording）
```

模块管理器按 level 分层初始化，同一层内按 deps 拓扑排序。

---

## 当前已注册 vs 未注册模块

### 已在 builtin.c 注册的内建模块

| 模块名 | 来源 | level | flags | 说明 |
|--------|------|-------|-------|------|
| `dbal` | `src/db/dbal_module.c` | EARLY | REQUIRED | 数据库抽象层 |
| `endpoints` | `src/mod/endpoints/endpoints.c` | WORKER_READY | REQUIRED | 分机/中继凭证库 |
| `outreg` | `src/mod/outbound_reg/outbound_reg.c` | WORKER_READY | - | 外呼注册 |
| `dialplan` | `src/mod/dialplans/dialplan.c` | WORKER_READY | - | 拨号计划 |
| `signaling` | `src/sip/signaling.c` | FINAL | CALL_PATH\|REQUIRED | SIP 核心信令 |
| `oam` | `src/oam/oam_module.c` | FINAL | - | 运维管理（UDS 命令） |
| `ha` | `src/ha/ha.c` | FINAL | - | 高可用 |
| `hep` | `src/media/bridge/hep_sink.c` | FINAL | - | HEP 抓包 |
| `recording` | `src/mod/recording/recording.c` | FINAL | - | 录音模块 |
| `billing` | `src/mod/billing/billing.c` | FINAL | - | 计费/CDR |

### 已写好但**未注册**的模块（仅存根 init，P1 真实现）

这些模块文件在 `src/mod/` 下，`NSBC_MOD0` 宏也写好了，但未在 builtin.c 里 extern + 入表，也未在 Makefile SRCS 里白名单追加。它们的 init 是空实现：

| 模块名 | 目录 | 用途 | 启用难度 |
|--------|------|------|----------|
| `applications` | `src/mod/applications/` | IVR / 会议 / 语音信箱 / 振铃组调度 | 中（需和 dialplan 接线） |
| `asr_tts` | `src/mod/asr_tts/` | 语音识别/合成 AI 桥 | 低（空 init，占位） |
| `cachedb` | `src/mod/cachedb/` | Redis/Memcached 外部缓存 | 低 |
| `codecs` | `src/mod/codecs/` | 外置编解码器插件 | 低 |
| `event_handlers` | `src/mod/event_handlers/` | 业务事件 handler 注册 | 低 |
| `formats` | `src/mod/formats/` | 文件格式（如 .wav/.mp3）插件 | 低 |
| `presence` | `src/mod/presence/` | 在线状态订阅 | 中 |
| `callcenter` | `src/mod/callcenter/` | 呼叫中心（agent/queue） | 高 |
| `security` | `src/mod/security/` | 防盗打/风控 | 高 |

启用方法：见 `02_enable_new_mods.cdiff`。

---

## 构建开关（Makefile 可选）

| 开关 | 默认 | 含义 |
|------|------|------|
| `NSBC_WITH_OPENSSL=1` | 开 | SIP TLS + DTLS-SRTP |
| `NSBC_WITH_SRTP=1` | 开 | libsrtp2 媒体加密 |
| `NSBC_WITH_SQLITE=1` | 关 | SQLite WAL 驱动 |
| `NSBC_WITH_DLOPEN=1` | 关 | 开发环境动态模块加载 |

示例：
```bash
make NSBC_WITH_OPENSSL=0            # 去掉 TLS
make NSBC_WITH_DLOPEN=1             # 启用 dlopen 开发态
```

`NSBC_WITH_DLOPEN` 开启后可以在运行时 `modmgr_load()` 加载 `.so`，但 `MOD_F_CALL_PATH` 模块会被拒绝 dlopen（呼叫面永不卸载）。

---

## 验证

```bash
make                                 # 编译
./nsbcd -T -c configs/nsbc.yaml      # 校验配置（dry-run 模式）
./nsbcd -c configs/nsbc.yaml          # 正常启动
./nsbc_cli module list                # 运行时查看模块加载情况
```

启动日志中应看到类似：
```
[INFO] core: registered 10 modules
[INFO] mod: inited dbal (level 0)
[INFO] mod: inited endpoints (level 3)
...
```
