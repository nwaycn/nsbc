---
title: "NIP 2.5.2 版本说明"
date: 2024-08-01
tags: NIP,2.5.2,更新日志
summary: "NIP 2.5.2 版本说明。"
slug: changelog-2.5.2
ord: 80
---
# NIP 高性能内核级 SIP 智能处理系统
## 使用说明书（v2.5.2）


**版本**：2.5.2（Smart Process Edition）

**配置基线**：nip.conf（2025.12.13 最新）

**适用系统**：CentOS 7 / Debian 10+ / OpenEuler 22.03+

**核心理念**：从 SIP 防护 → 识别 → 决策 → 执行 的完整业务闭环

---

## 吐槽一下
坑爹的`cargo build --release --target x86_64-unknown-linux-musl`, 全网都说构建完了，无需glibc支持，支持任何系统，**无依赖**。但实际上，在centos 7.9, debian 12, OpenEuler 22.03+ 上运行都是正常的，但是版本再低一些如CentOS 7.6, 就跑不起来了。还是golang的跨平台build更适用于更多平台。

## 1. 系统概述

**NIP（Nway IP Process System）** 是一套基于 Linux 内核模块的高性能 SIP 流量分析与智能处理系统。

在 **2.5 版本**中，系统架构从传统的“防护 / 录音工具”升级为：

> **内核识别 + 用户态检测 + 独立决策进程（Safer Manager）+ 软交换联动**

系统可在毫秒级内完成以下动作：

- 内核层拦截恶意 SIP / RTP 流量
- 用户态进行 RTP 音频检测（CPD）
- 识别空号 / 忙音 / 欠费 / 无人接听等状态
- 主动调用 VOS / FreeSWITCH 接口执行快速挂机
- 同步写入数据库、推送事件、（可选）录音与 HEP
- 支持用户随间定义任意挂机的早期媒体语音

---

## 2. 系统组件与职责

| 组件 | 运行态 | 职责 |
|----|----|----|
| nip_kmod.ko | 内核态 | SIP/RTP 捕获、IP/UA/号码过滤、数据分发 |
| nip_ctl | 用户态 | 控制中枢，加载内核模块、启动各服务 |
| nip_logd | 用户态 | 统一日志收集（内核 + 用户态） |
| nip_recdd | 用户态 | SIP 解析、RTP 录音、CPD 检测、事件产生 |
| safer_manager | 用户态 | 决策引擎：快挂、入库、软交换联动 |
| nip_pcapd | 用户态 | 按需抓包（PCAP） |

---

## 3. 系统整体数据流（文字示意）

```
SIP / RTP 流量
      │
      ▼
[ nip_kmod.ko ]
  │  IP / UA / 号码过滤
  │
  ▼
[ nip_recdd ]
  │  ├─ CPD 音频检测
  │  ├─（record 模式）录音 / 话单
  │  ├─（可选）HEP 推送
  │
  ▼
[ safer_manager ]
  │  ├─ 快速挂机（VOS / FS）
  │  └─ 数据库写入
```

---

## 4. 工作模式说明（capture_type）

配置项：`[general] capture_type`

| 模式 | 行为说明 |
|----|----|
| cpd | 仅检测模式：仅进行 RTP 音频检测，不生成完整录音与 CDR |
| record | 全功能模式：全量录音、CPD、CDR、事件、支持 HEP |
| capture | 抓包模式：仅生成 pcap 文件，用于排障 |

> 说明：HEP 与 CPD 的深度联动目前主要在 record 模式下使用。

---

## 5. 核心配置文件说明

### nip.conf

#### 5.1 [general]

| 参数 | 说明 |
|----|----|
| local_ip | 本机 IP |
| packet_worker_threads | 包处理线程数 |
| interface | 抓包网卡 |
| capture_type | 工作模式 |
| debug | 调试开关 |

#### 5.2 [paths]

| 参数 | 说明 |
|----|----|
| custom_white_ip_file | IP 白名单 |
| ip_black_file | IP 黑名单 |
| region_white_ip_file | 地域白名单 |
| src_blacklist_file | 主叫黑名单 |
| dst_blacklist_file | 被叫黑名单 |
| ua_black_file | UA 黑名单 |
| ua_white_file | UA 白名单 |
| license_file | 授权文件 |
| recording_path | 录音路径 |

#### 5.3 [filter]

| 参数 | 说明 |
|----|----|
| sip_port_ranges | SIP 端口 |
| allow_ua_empty | 允许空 UA |
| ua_strict | 严格 UA |
| debug | 调试日志 |

#### 5.4 [pcapd]

| 参数 | 说明 |
|----|----|
| pcap_dir | pcap 目录 |
| pcap_callback_url | 回调地址 |
| pcap_callback_enabled | 是否启用 |
| io_threads | IO 线程 |

#### 5.5 [recording]

| 参数 | 说明 |
|----|----|
| recording_callback_url | CDR 回调 |
| recording_callback_enabled | 是否推送 |
| idle_hang_seconds | 空闲挂机 |
| b2bua_only_a_leg | 仅 A-leg |
| cdr_batch_enabled | 批量推送 |
| cdr_batch_size | 批量大小 |
| wav_channels | 声道 |
| writer_threads | 写盘线程 |

#### 5.6 [cpd]

| 参数 | 说明 |
|----|----|
| enabled | 启用 CPD |
| recording_enabled | CPD 录音 |
| sample_dir | 样本目录 |
| ringback_detection_enabled | 回铃检测 |
| similarity_threshold | 相似度 |
| recording_dir | CPD 录音目录 |
| safer_manager_enabled | 启用决策 |
| safer_manager_url | 决策地址 |

#### 5.7 [hep]

| 参数 | 说明 |
|----|----|
| enable | 是否启用 |
| host | HEP 主机 |
| port | HEP 端口 |
| capture_id | 节点 ID |
| log_sip | SIP |
| log_rtp | RTP |
| log_rtcp | RTCP |
| queue_size | 队列 |
| workers | 线程 |
| drop_on_full | 满即丢弃 |
| bind_ip | 绑定 IP |

---

## 6. Safer Manager

Safer Manager 是 2.5.2 版本的控制核心组件，用于：

- 接收 CPD 检测结果
- 判断是否自动挂机
- 调用软交换接口
- 写入数据库

---

## 7. 快挂流程

1. 内核捕获 SIP / RTP
2. 内核安全过滤
3. CPD 音频检测
4. 状态识别
5. 上报 Safer Manager
6. 执行挂机与入库

---

## 8. 运维操作

### 启动

```bash
./nip_ctl init
./safer_manager &
```

### 抓包

```bash
./nip_ctl --pcap-enable 1 --pcap-mask 3
./nip_ctl --pcap-enable 0
```

### 查看内核是否工作
```
tail -f /var/log/nip_kmod.log
```

### 查看用户态是否工作
cpd为例
```bash
tail -f /$PWD/safer_manager/logs/sm.log
```
---

## 9. FAQ

**Q：为什么 cpd 没有话单？**
A：cpd 为高性能检测模式，不生成完整 CDR。

**Q：为什么需要 Safer Manager？**
A：实现检测与执行解耦，提升系统稳定性与扩展性。

**Q：CPD 误判怎么办？**
A：调整 similarity_threshold 参数，或开启 ringback_detection_enabled 回铃检测。

**Q：高危配置项提醒**

| 配置 | 风险 |
|----|----|
| ua_strict=1 | 易误拦截 |
| similarity_threshold 过高 | CPD 误判 |
| HEP 全开 | IO 压力 |
| writer_threads 过低 | 录音阻塞 |

---

## 11. 恢复与重启建议

### 推荐顺序

1. 重启 safer_manager
2. 重启 nip_ctl

```bash
./nip_ctl init
./nip_ctl --pcap-enable 1 --pcap-mask 3
./safer_manager &
```

## 12. 文档信息

- 系统版本：NIP 2.5.2
- 文档类型：Markdown 使用说明书
- 维护方：上海宁卫信息技术有限公司
