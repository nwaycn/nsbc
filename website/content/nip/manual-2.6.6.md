---
title: "NIP 2.6.6 系统说明书"
date: 2026-01-20
tags: NIP,说明书,2.6.6,CPD,safer_manager
summary: "NIP 2.6.6 完整运行说明：nip_kmod 内核模块、nip_cpd 包检测、safer_manager 挂机决策、cpd_web 可视化。内核级 SIP/RTP 抓包检测 B-leg Local IP 实现早媒体检测。"
slug: manual-2.6.6
ord: 20
---
## 1. 系统概述
**NIP（Nway IP Process System）** 是一套基于 Linux 内核模块的高性能 SIP/RTP 流量分析与智能处理系统。
自 **2.6.6 版本** 起，系统架构进一步简化为“检测 + 快挂 + 日志 + 可视化”核心链路，去除了录音、HEP、话单等非必要模块。

系统核心目标为：
- 内核级 SIP / RTP 抓取
- B-leg Local IP RTP 包检测（CPD）
- 快速挂机（Safer Manager）
- 内核日志用户态记录（nip_logd）
- Web 端话单可视化与统计（cpd_web）

---

## 2. 系统组件与职责

| 组件 | 运行态 | 职责说明 |
|------|--------|----------|
| **nip_kmod.ko** | 内核态 | 捕获 SIP/RTP 流量，完成 IP/UA/号码过滤，将数据分发至 nip_cpd 与 nip_logd |
| **nip_ctl** | 用户态 | 系统控制入口：加载内核模块、初始化服务、启停抓包 |
| **nip_logd** | 用户态 | 仅记录来自 **nip_kmod.ko** 的日志信息 |
| **nip_cpd** | 用户态 | 接收内核 RTP 包，仅检测 **B-leg local IP**，并上报结果给 safer_manager |
| **safer_manager** | 用户态 | 根据检测结果执行挂机、决策入库（数据库 cpd_records 表） |
| **cpd_web** | 用户态 | 提供 Web 可视化界面：查询、统计、导出 CPD 记录 |

---

## 3. 系统数据流示意

```
SIP / RTP 流量
      │
      ▼
[ nip_kmod.ko ]
  ├─► [ nip_logd ]         → 记录内核日志
  └─► [ nip_cpd ]          → RTP 检测（B-leg local IP）
           │
           ▼
     [ safer_manager ]
           ├─ 快速挂机（VOS / FS）
           ├─ 数据入库 (cpd_records)
           ▼
        [ cpd_web ]
          ├─ 登录 / 验证码
          ├─ 话单查询与导出
          └─ 落地去重统计
```

---

## 4. 工作模式说明（capture_type）

配置项：`[general] capture_type`

| 模式 | 行为说明 |
|------|----------|
| **cpd** | 仅检测模式：RTP 包检测与挂机执行，不生成录音、CDR、HEP，启动nip_cpd应用 |
| **capture** | 抓包模式：仅保存 PCAP 文件用于排障 |
| **record** | 录音模式：保存录音文件并推送HEP且自动启动nip_recdd应用 |

> **说明**：2.6.6 版本仅使用 `capture_type=cpd`。

---

## 5. 核心配置文件说明（nip.conf）
**切记：**
local_ip=10.0.0.120,39.98.34.45这样把本机的内网及公网ip都可以写在里边
interface=eth0 这是指定我们要处理的内核包的网卡，必须要指定
**另**
这是linux操作系统层的应用，需要uname -a查看内核版本，然后和我们的编译好的内核模块版本一定要一致。

### 5.1 `[general]`
| 参数 | 说明 |
|------|------|
| local_ip | 本机 IP |
| interface | 抓包网卡 |
| capture_type | 工作模式（固定为 `cpd`） |
| packet_worker_threads | 包处理线程数 |
| debug | 调试日志开关 |

### 5.2 `[paths]`
| 参数 | 说明 |
|------|------|
| custom_white_ip_file | IP 自定义白名单文件 |
| ip_black_file | IP 黑名单文件 |
| region_white_ip_file | 地域自定义白名单文件 |
| ua_black_file | UA 黑名单文件 |
| ua_white_file | UA 自定义白名单文件 |
| src_black_file | SIP 源主叫 黑名单文件 |
| dst_black_file | SIP 目的被叫 黑名单文件 |
| license_file | 授权文件 |


### 5.3 `[cpd]`
| 参数 | 说明 |
|------|------|
| enabled | 是否启用 CPD（1=启用） |
| ringback_detection_enabled | 是否启用回铃检测 |
| similarity_threshold | 相似度阈值（默认 0.75） |
| safer_manager_enabled | 是否启用挂机决策（1=启用） |
| safer_manager_url | Safer Manager 地址 |

### 5.4 `[pcapd]`
| 参数 | 说明 |
|------|------|
| pcap_dir | 抓包文件目录 |
| io_threads | 抓包线程数 |

---

## 6. Safer Manager 模块说明

**Safer Manager** 是 NIP 2.6.6 的挂机与数据决策核心，主要功能如下：
- 接收 **nip_cpd** 上报的检测结果；
- 根据规则判断是否执行挂机；
- 调用软交换接口（VOS / FreeSWITCH）；
- 将挂机结果写入数据库表 `cpd_records`；
- 向 **cpd_web** 提供统一查询接口；
- 独立日志输出，不经过 nip_logd。

---

## 7. cpd_web 模块说明

**cpd_web** 是基于 **Golang + Gin + Layui** 开发的轻量级 Web 可视化平台。
它与 Safer Manager 共用数据库，可在 Web 界面上查看与分析历史回铃检测记录。

### 7.1 功能概述
1. **登录认证**
   - 用户名 + 密码 + 验证码
   - 支持 Session 管理
   - 用户表：`users`

2. **话单查询（菜单：话单查询）**
   - 按时间段、主叫、被叫、IP、`to_ip:to_port` 查询；
   - 按 `type_str` 条件筛选（空号、不在服务区、欠费、停机等）；
   - 分页显示；
   - 支持 CSV 导出。

3. **落地去重（菜单：落地去重）**
   - 按时间段、`to_ip:to_port` 聚合统计；
   - 按 `type_str` 分类汇总；
   - 可导出结果为 CSV。

4. **数据维护**
   - 数据表：`cpd_records`
   - 数据库：MySQL / PostgreSQL
   - 数据自动清理周期由配置项 `keep_days` 控制。

---

### 7.2 配置文件（config.ini）
```ini
[server]
port = 8080
session_secret = nip_cpd_web_secret

[database]
enable = true
driver = postgres
dsn = host=127.0.0.1 user=postgres dbname=nip port=5432 sslmode=disable
keep_days = 30
```

---

### 7.3 数据库结构

#### cpd_records
```sql
CREATE TABLE IF NOT EXISTS cpd_records (
    id BIGSERIAL PRIMARY KEY,
    call_id VARCHAR(64) NOT NULL,
    type_str VARCHAR(32) DEFAULT '',
    similarity DOUBLE PRECISION DEFAULT 0,
    caller VARCHAR(32) NOT NULL,
    destination VARCHAR(32) DEFAULT '',
    to_ip VARCHAR(32) DEFAULT '',
    to_port INT DEFAULT 0,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);
```

#### users
```sql
CREATE TABLE IF NOT EXISTS users (
    id SERIAL PRIMARY KEY,
    username VARCHAR(50) NOT NULL UNIQUE,
    password VARCHAR(255) NOT NULL,
    created_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP WITH TIME ZONE DEFAULT CURRENT_TIMESTAMP
);
```

---

## 8. 工作流程

1. **初始化系统**
   ```bash
   ./nip_ctl init
   ```
2. **启用抓包**
   ```bash
   ./nip_ctl --pcap-enable 1 --pcap-mask 3
   ```
3. **启动后台服务**
   ```bash
   ./safer_manager &
   ./cpd_web &
   ```
4. **系统运行链路**
   ```
   内核捕获 → nip_logd 记录日志
             → nip_cpd 检测 → safer_manager 挂机入库
             → cpd_web 展示
   ```

---

## 9. 运维操作

### 查看状态
```bash
tail -f /var/log/nip_kmod.log            # 内核日志
tail -f ./nip_cpd/logs/cpd.log           # CPD 日志
tail -f ./safer_manager/logs/sm.log      # Safer Manager 日志
tail -f ./cpd_web/logs/web.log           # Web 日志
```

### 停止抓包
```bash
./nip_ctl --pcap-enable 0
```

### 恢复与重启建议
```bash
pkill safer_manager
pkill nip_cpd
pkill cpd_web
./nip_ctl init
./nip_ctl --pcap-enable 1 --pcap-mask 3
./safer_manager &
./cpd_web &
```

---

## 10. FAQ

| 问题 | 说明 |
|------|------|
| Q：为什么没有录音或 CDR？ | 因为 2.6.6 版本仅保留检测与快挂功能，录音与话单模块已移除。 |
| Q：为什么必须启用 Safer Manager？ | CPD 仅检测，挂机与入库均由 Safer Manager 完成。 |
| Q：nip_logd 是否记录用户态日志？ | 否，仅记录内核模块（nip_kmod）的日志输出。 |
| Q：Web 登录失败？ | 检查 `users` 表中是否存在账号；默认密码需加密存储。 |
| Q：CSV 导出在哪里？ | Web 页面顶部“导出”按钮，可导出当前查询条件结果。 |

---

## 11. 版本与维护信息

| 项目 | 内容 |
|------|------|
| 系统版本 | **NIP 2.6.6** |
| 文档类型 | 系统运行说明书 |
| 维护方 | 上海宁卫信息技术有限公司 |
| 更新时间 | 2026-01 |
