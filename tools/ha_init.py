#!/usr/bin/env python3
"""
NSBC HA 配置生成器 (P2-8) — 纯客户端工具，零 nsbcd 依赖。

用法:
    python3 tools/ha_init.py --node-a 10.0.0.80 --node-b 10.0.0.81 \\
        --vip 139.226.66.211 --interface eth0

输出到 stdout:
    [nsbc.yaml ha 段] node-a
    [nsbc.yaml ha 段] node-b
    [keepalived.conf] node-a (MASTER)
    [keepalived.conf] node-b (BACKUP)
    [验证 checklist]

可选输出目录:
    python3 tools/ha_init.py ... --output-dir ./ha-out
    → ./ha-out/node-a.yaml, node-b.yaml, node-a-keepalived.conf,
      node-b-keepalived.conf, README.md

生成规则:
    - 所有参数走合理默认（heartbeat_ms=500, failover_ms=3000 等）
    - P1-5 自动联动: ha.enabled=true + 有 peer → replica 默认开启
    - P1-6 自动联动: bfd.enabled=true 未显式配 failover_ms → 自动调 failover_ms
    - BFD 默认关闭, replica 默认开启（除非 --no-replica）
    - keepalived priority 自动差 10, 可用 --a-prio/--b-prio 覆盖
"""

from __future__ import annotations
import argparse
import ipaddress
import os
import sys
import textwrap
from pathlib import Path


# ---------------------------------------------------------------------------
# 常量默认值（与 cfg_schema.c 一致）
# ---------------------------------------------------------------------------

DEFAULTS = {
    # 基础心跳
    "ha.enabled": True,
    "ha.heartbeat_ms": 500,
    "ha.failover_ms": 3000,
    # replica
    "ha.replica.enabled": True,   # P1-5 自动联动后默认开
    "ha.replica.port": 5071,
    "ha.replica.ring_cap": 8192,
    "ha.replica.payload_max": 4096,
    "ha.replica.lag_warn": 10000,
    "ha.replica.full_resync_interval": 0,
    # BFD
    "ha.bfd.enabled": False,
    "ha.bfd.desired_min_tx_interval": 200,
    "ha.bfd.required_min_rx_interval": 200,
    "ha.bfd.detect_multiplier": 3,
    # Hitless
    "ha.hitless.enabled": False,
    "ha.hitless.freeze_ms": 1000,
    "ha.hitless.upgrade_timeout_ms": 10000,
}


# ---------------------------------------------------------------------------
# nsbc.yaml ha 段生成
# ---------------------------------------------------------------------------

def _yaml_bool(v: bool) -> str:
    return "true" if v else "false"


def _yaml_commented(v: bool, comment: str) -> str:
    """用于'默认开启时不写、显式关才写'的配置 — 减少用户噪音"""
    if v:
        return f"{comment}"
    return f"# {comment}  # 当前关闭"


def build_ha_yaml(
    peer_ip: str,
    peer_port: int,
    my_priority: int,
    *,
    heartbeat_ms: int = DEFAULTS["ha.heartbeat_ms"],
    failover_ms: int | None = None,   # None = 让 P1-6 自动计算
    replica_enabled: bool = DEFAULTS["ha.replica.enabled"],
    replica_port: int = DEFAULTS["ha.replica.port"],
    bfd_enabled: bool = DEFAULTS["ha.bfd.enabled"],
    bfd_tx: int = DEFAULTS["ha.bfd.desired_min_tx_interval"],
    bfd_rx: int = DEFAULTS["ha.bfd.required_min_rx_interval"],
    bfd_mult: int = DEFAULTS["ha.bfd.detect_multiplier"],
    hitless_enabled: bool = DEFAULTS["ha.hitless.enabled"],
) -> str:
    """
    生成 nsbc.yaml 的 ha: 段 (2-space indent, 直接贴到 nsbc.yaml 顶层 ha: 下面).

    策略:
    - 默认值**省略**不写（让 cfg_schema 走默认）— 最小化用户配置体积
    - 只有非默认值才显式写出
    - BFD 开启 + failover_ms=None → 按 P1-6 规则自动计算 failover_ms 写入
    """
    lines = []

    # ---- 基础 ----
    lines.append("ha:")
    lines.append(f"  enabled: true")
    lines.append(f"  peer: \"{peer_ip}:{peer_port}\"")

    # priority 一定显式写 — 文档 L10 强调"显式主高备低，避免 hostname 不确定性"
    lines.append(f"  priority: {my_priority}")

    # heartbeat_ms / failover_ms 只有非默认才写
    if heartbeat_ms != DEFAULTS["ha.heartbeat_ms"]:
        lines.append(f"  heartbeat_ms: {heartbeat_ms}")

    # P1-6: BFD 自动联动 failover_ms
    if failover_ms is None:
        if bfd_enabled:
            bfd_win = max(bfd_tx, bfd_rx) * bfd_mult
            failover_ms = bfd_win + 500
        else:
            failover_ms = DEFAULTS["ha.failover_ms"]

    if failover_ms != DEFAULTS["ha.failover_ms"]:
        lines.append(f"  failover_ms: {failover_ms}")

    # ---- status replication ----
    if replica_enabled:
        lines.append("  replica:")
        lines.append("    enabled: true")
        if replica_port != DEFAULTS["ha.replica.port"]:
            lines.append(f"    port: {replica_port}")
        # ring_cap / payload_max / lag_warn / full_resync_interval 走默认就省略
        # 用户想覆盖时自己加，保持极简

    # ---- BFD ----
    if bfd_enabled:
        lines.append("  bfd:")
        lines.append("    enabled: true")
        if bfd_tx != DEFAULTS["ha.bfd.desired_min_tx_interval"]:
            lines.append(f"    desired_min_tx_interval: {bfd_tx}")
        if bfd_rx != DEFAULTS["ha.bfd.required_min_rx_interval"]:
            lines.append(f"    required_min_rx_interval: {bfd_rx}")
        if bfd_mult != DEFAULTS["ha.bfd.detect_multiplier"]:
            lines.append(f"    detect_multiplier: {bfd_mult}")

    # ---- Hitless Upgrade ----
    if hitless_enabled:
        lines.append("  hitless:")
        lines.append("    enabled: true")

    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# keepalived.conf 生成
# ---------------------------------------------------------------------------

def build_keepalived_conf(
    *,
    state: str,             # "MASTER" | "BACKUP"
    priority: int,          # 150 | 140
    vip: str,
    interface: str,
    peer_port: int = 5070,  # vrrp 心跳端口不用这个；这个是 nsbcd 心跳 UDP，供 track_service 参考
    auth_pass: str = "nsbc_ha",
    router_id: str = "NSBC_HA",
) -> str:
    """
    生成 keepalived.conf。state + priority 两节点唯一区别。
    所有其他字段一致。track_process nsbcd 确保 nsbcd 挂了自动摘 VIP。
    """
    vrf_id = 51
    # VIP 默认补 /24（nsbc 双机部署场景最常见的 C 段）；用户自己带前缀就用用户的
    if "/" in vip:
        vip_with_mask = vip
    else:
        vip_with_mask = vip + "/24"

    conf = textwrap.dedent(f"""\
    # keepalived.conf — NSBC HA 双机 VIP 漂移
    # 本文件为 {'MASTER' if state == 'MASTER' else 'BACKUP'} 节点版本
    # 安装路径参考: /etc/keepalived/keepalived.conf

    global_defs {{
        router_id {router_id}
        # 日志: /var/log/messages 或 systemd journal
        vrrp_garp_master_delay 1
        vrrp_garp_master_repeat 3
    }}

    # ---- VIP 漂移 ----
    vrrp_instance VI_1 {{
        state {state}
        interface {interface}
        virtual_router_id {vrf_id}
        priority {priority}
        advert_int 1
        authentication {{
            auth_type PASS
            auth_pass {auth_pass}
        }}
        virtual_ipaddress {{
            {vip_with_mask} dev {interface} label {interface}:0
        }}
        # nsbcd 进程挂了自动摘 VIP —— 比 HA 心跳更硬的保险
        track_process {{
            nsbcd weight -30
        }}
        # 网关可达性检测（可选，注释掉默认跳过）
        # track_script {{
        #     check_gw weight -10 interval 2 timeout 2 fall 2 rise 1
        # }}
    }}

    # ---- track_process: nsbcd 进程存活 ----
    vrrp_track_process {{
        nsbcd process nsbcd
    }}

    # ---- 网关探测脚本模板（可选） ----
    # vrrp_script check_gw {{
    #     script "/usr/local/bin/check_gw.sh"
    #     interval 2
    #     timeout 2
    #     fall 2
    #     rise 1
    # }}
    """)
    return conf


# ---------------------------------------------------------------------------
# 30 秒验证 checklist
# ---------------------------------------------------------------------------

CHECKLIST_TEMPLATE = textwrap.dedent("""\
    ============================================================
    NSBC HA 部署验证 Checklist
    ============================================================

    [1] 防火墙开端口（两台都要）
        firewall-cmd --permanent --add-port=5060/udp   # SIP
        firewall-cmd --permanent --add-port=5070/udp   # HA 心跳
        firewall-cmd --permanent --add-port=5071/tcp   # 状态复制（replica）
        firewall-cmd --reload

    [2] 启动顺序无所谓 — 同时或单先后都行
        systemctl start nsbcd
        systemctl start keepalived

    [3] HA 启动日志（两台都应看到）
        grep 'heartbeat enabled' /var/log/nsbcd.log
        → NOTICE ha: heartbeat enabled peer=X:5070 hb=500ms failover=Nms prio=M node=NAME

    [4] 角色收敛 — 主节点看到 role STANDBY -> ACTIVE
        grep 'role.*-> ACTIVE' /var/log/nsbcd.log
        → 主节点: NOTICE ha: role STANDBY -> ACTIVE ...
        → 备节点: 无（保持 STANDBY）

    [5] VIP 漂移 — 主节点 VIP 在接口上
        ip addr show dev <interface>
        → 看到 VIP/24

    [6] 状态复制（如果 replica.enabled=true）
        nsbc_cli ha status
        → Replica connected: yes
          lag: 0

    [7] failover 测试 — 主节点 kill nsbcd
        killall nsbcd
        # 3 秒后 (failover_ms) 备节点:
        grep 'role.*-> ACTIVE' /var/log/nsbcd.log
        → NOTICE ha: role STANDBY -> ACTIVE (peer down/absent ...)
        # keepalived VIP 漂到备节点:
        ip addr show dev <interface>   # 备节点看到 VIP
        # UA INVITE 继续可达 (如果 UA 用 VIP 做目标)

    [8] 恢复测试 — 主节点重新启动
        systemctl start nsbcd
        # 备节点: role ACTIVE -> STANDBY (peer alive + priority tie-break)
        # VIP 漂回主节点

    ============================================================
    命令快捷方式 (nsbc_cli 已注册):
        ha status             # 综合状态一屏看
        ha status-frozen      # FREEZE 状态
        ha hitless_upgrade    # 触发滚动升级 (需 ha.hitless.enabled=true)
    ============================================================
""")


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------

def _validate_args(args) -> None:
    """参数互斥/依赖校验。提前失败比生成一堆垃圾 YAML 好。"""
    errs = []
    try:
        ipaddress.ip_address(args.node_a)
    except ValueError:
        errs.append(f"--node-a '{args.node_a}' 不是合法 IP")
    try:
        ipaddress.ip_address(args.node_b)
    except ValueError:
        errs.append(f"--node-b '{args.node_b}' 不是合法 IP")
    if args.vip:
        try:
            ipaddress.ip_address(args.vip.split("/")[0])
        except ValueError:
            errs.append(f"--vip '{args.vip}' 不是合法 IP")

    # priority 必须不同（否则 hostname tie-break 不确定谁是主）
    if args.a_prio == args.b_prio:
        errs.append(f"--a-prio 和 --b-prio 必须不同（当前都是 {args.a_prio}）")

    if args.node_a == args.node_b:
        errs.append("--node-a 和 --node-b 不能相同")

    if errs:
        print("[ERROR] 参数校验失败:\n  - " + "\n  - ".join(errs), file=sys.stderr)
        sys.exit(2)


def main() -> None:
    ap = argparse.ArgumentParser(
        prog="ha_init.py",
        description="NSBC HA 双机配置生成器 — 输出双边 nsbc.yaml ha 段 + keepalived.conf",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
            示例:
              python3 tools/ha_init.py \\
                --node-a 10.0.0.80 --node-b 10.0.0.81 \\
                --vip 139.226.66.211 --interface eth0 \\
                --output-dir ./ha-out

            默认值与 cfg_schema.c 一致。P1-5 自动 replica 联动、P1-6 BFD 自动
            failover_ms 联动 — 用户只需填最基础的 IP。
        """),
    )
    ap.add_argument("--node-a", required=True, help="主节点 IP (例 10.0.0.80)")
    ap.add_argument("--node-b", required=True, help="备节点 IP (例 10.0.0.81)")
    ap.add_argument("--vip", required=False, help="Keepalived VIP (例 139.226.66.211) — 不填则跳过 keepalived 输出")
    ap.add_argument("--interface", default="eth0", help="VIP 绑定物理网卡 (默认 eth0)")
    ap.add_argument("--a-prio", type=int, default=150, help="节点 A priority (默认 150, 高=主)")
    ap.add_argument("--b-prio", type=int, default=140, help="节点 B priority (默认 140, 低=备)")
    ap.add_argument("--ha-port", type=int, default=5070, help="HA 心跳 UDP 端口 (默认 5070)")
    ap.add_argument("--repl-port", type=int, default=5071, help="状态复制 TCP 端口 (默认 5071)")
    ap.add_argument("--no-replica", action="store_true", help="关闭状态复制 (默认开启, P1-5 联动)")
    ap.add_argument("--bfd", action="store_true", help="启用 BFD 亚秒探测 (默认关闭)")
    ap.add_argument("--hitless", action="store_true", help="启用 Hitless Upgrade 滚动升级 (默认关闭)")
    ap.add_argument("--heartbeat-ms", type=int, default=DEFAULTS["ha.heartbeat_ms"],
                    help=f"心跳发送间隔 (默认 {DEFAULTS['ha.heartbeat_ms']}ms)")
    ap.add_argument("--failover-ms", type=int, default=None,
                    help="failover 窗口 (默认由 P1-6 自动算, 无 BFD 则 3000ms)")
    ap.add_argument("--output-dir", default=None,
                    help="输出目录 (例 ./ha-out). 不填则打印到 stdout")
    args = ap.parse_args()
    _validate_args(args)

    # ---- 生成两边 nsbc.yaml ha 段 ----
    ha_a = build_ha_yaml(
        peer_ip=args.node_b, peer_port=args.ha_port,
        my_priority=args.a_prio,
        heartbeat_ms=args.heartbeat_ms,
        failover_ms=args.failover_ms,
        replica_enabled=not args.no_replica,
        replica_port=args.repl_port,
        bfd_enabled=args.bfd,
        hitless_enabled=args.hitless,
    )
    ha_b = build_ha_yaml(
        peer_ip=args.node_a, peer_port=args.ha_port,
        my_priority=args.b_prio,
        heartbeat_ms=args.heartbeat_ms,
        failover_ms=args.failover_ms,
        replica_enabled=not args.no_replica,
        replica_port=args.repl_port,
        bfd_enabled=args.bfd,
        hitless_enabled=args.hitless,
    )

    # ---- 生成 keepalived.conf (只有 VIP 填了才生成) ----
    ka_a = ka_b = None
    if args.vip:
        ka_a = build_keepalived_conf(
            state="MASTER", priority=args.a_prio, vip=args.vip,
            interface=args.interface,
        )
        ka_b = build_keepalived_conf(
            state="BACKUP", priority=args.b_prio, vip=args.vip,
            interface=args.interface,
        )

    # ---- 输出 ----
    if args.output_dir:
        out = Path(args.output_dir)
        out.mkdir(parents=True, exist_ok=True)
        (out / "node-a.yaml").write_text(ha_a, encoding="utf-8")
        (out / "node-b.yaml").write_text(ha_b, encoding="utf-8")
        if ka_a and ka_b:
            (out / "node-a-keepalived.conf").write_text(ka_a, encoding="utf-8")
            (out / "node-b-keepalived.conf").write_text(ka_b, encoding="utf-8")
        (out / "README.md").write_text(_build_readme(args), encoding="utf-8")
        print(f"[OK] 输出到 {out.resolve()}/")
        print(f"     node-a.yaml               ← 贴到 {args.node_a} 的 nsbc.yaml")
        print(f"     node-b.yaml               ← 贴到 {args.node_b} 的 nsbc.yaml")
        if args.vip:
            print(f"     node-a-keepalived.conf   ← {args.node_a} /etc/keepalived/keepalived.conf")
            print(f"     node-b-keepalived.conf   ← {args.node_b} /etc/keepalived/keepalived.conf")
        print(f"     README.md                 ← 部署说明 + 30 秒验证 checklist")
    else:
        _print_stdout(args, ha_a, ha_b, ka_a, ka_b)


def _print_stdout(args, ha_a, ha_b, ka_a, ka_b) -> None:
    """stdout 打印: 加清晰分隔符，方便复制到终端直接粘贴"""
    print("=" * 64)
    print("  NSBC HA 配置生成器 (P2-8)")
    print("=" * 64)
    print(f"  node-A: {args.node_a} (MASTER, prio={args.a_prio})")
    print(f"  node-B: {args.node_b} (BACKUP, prio={args.b_prio})")
    if args.vip:
        print(f"  VIP:    {args.vip}  iface={args.interface}")
    else:
        print("  VIP:    (未填, 仅生成 nsbc.yaml ha 段)")
    print("=" * 64)

    print("\n--- node-a: nsbc.yaml (贴到 nsbc.yaml 的 ha: 段) ---")
    print(ha_a.rstrip())
    print("\n--- node-b: nsbc.yaml ---")
    print(ha_b.rstrip())

    if ka_a and ka_b:
        print("\n--- node-a: /etc/keepalived/keepalived.conf ---")
        print(ka_a.rstrip())
        print("\n--- node-b: /etc/keepalived/keepalived.conf ---")
        print(ka_b.rstrip())

    print("\n--- 30 秒验证 Checklist ---")
    print(CHECKLIST_TEMPLATE)


def _build_readme(args) -> str:
    return textwrap.dedent(f"""\
    # NSBC HA 双机配置 — {args.node_a} ↔ {args.node_b}

    由 `tools/ha_init.py` 自动生成。**先读再动手，避免生产踩坑**。

    ## 快速三步

    1. **贴配置**：node-a.yaml 的内容贴到 `{args.node_a}:/path/to/nsbc.yaml` 的 `ha:` 段（或新建顶层 ha:）；node-b.yaml 同理到 {args.node_b}
    2. **装 keepalived**（如果用 VIP 漂移）：`yum install -y keepalived` / `apt install keepalived`，把 node-*-keepalived.conf 复制到 `/etc/keepalived/keepalived.conf`
    3. **开防火墙**：5060/udp (SIP), 5070/udp (HA 心跳), 5071/tcp (状态复制)

    ```bash
    firewall-cmd --permanent --add-port=5060/udp
    firewall-cmd --permanent --add-port=5070/udp
    firewall-cmd --permanent --add-port=5071/tcp
    firewall-cmd --reload
    ```

    ## 启动

    ```bash
    # 两台各跑一遍 — 顺序无所谓
    systemctl start nsbcd
    systemctl start keepalived   # 如果用 VIP
    ```

    ## 验证（30 秒 checklist）

    ```bash
    # [1] HA 启动日志
    grep 'heartbeat enabled' /var/log/nsbcd.log
    # node-a: NOTICE ha: heartbeat enabled peer={args.node_b}:{args.ha_port} hb={DEFAULTS['ha.heartbeat_ms']}ms ...

    # [2] 角色收敛
    grep 'role' /var/log/nsbcd.log
    # node-a: NOTICE ha: role STANDBY -> ACTIVE ...  ← 主节点看到这条
    # node-b: (保持 STANDBY)

    # [3] VIP (如果用 keepalived)
    ip addr show dev {args.interface}
    # node-a: 看到 {args.vip}/24

    # [4] nsbc_cli 一屏看全
    nsbc_cli ha status
    ```

    ## failover 测试

    ```bash
    # 主节点
    killall nsbcd
    # 3 秒后备节点:
    grep 'role.*-> ACTIVE' /var/log/nsbcd.log
    # → NOTICE ha: role STANDBY -> ACTIVE (peer down/absent ... reason=peer_down_failover)
    # keepalived VIP 漂到备节点
    # UA INVITE 继续可达 (如果用 VIP 做目标)

    # 恢复
    systemctl start nsbcd   # 备节点也会看到 role ACTIVE -> STANDBY
    ```

    ## 配置参数说明

    | 参数 | 默认 | 生成值 | 作用 |
    |------|------|--------|------|
    | heartbeat_ms | {DEFAULTS['ha.heartbeat_ms']} | {args.heartbeat_ms} | 心跳发送间隔，仲裁周期 = 它/4 |
    | failover_ms | {args.failover_ms or ('P1-6 自动 (BFD 开时) / ' + str(DEFAULTS['ha.failover_ms']))} | — | 对端 down 多久我接管 |
    | replica | 开 | {'开' if not args.no_replica else '关'} | 状态复制 (P1-5 自动联动) |
    | bfd | 关 | {'开' if args.bfd else '关'} | BFD 亚秒探测 |
    | hitless | 关 | {'开' if args.hitless else '关'} | Hitless Upgrade |

    全部参数可热加载（除 enabled / peer / bfd.enabled / replica.enabled 等需重启），详见 `conf/ha配置.md` §配置变更生效方式。

    ## 源

    - `conf/ha配置.md` — 完整设计文档
    - `src/ha/ha.c` — 心跳/仲裁/Hitless Upgrade 实现
    - `src/ha/ha_repl_transport.c` — 状态复制 TCP transport
    - `src/ha/ha_bfd.c` — BFD RFC 5881 实现
    """)


if __name__ == "__main__":
    main()
