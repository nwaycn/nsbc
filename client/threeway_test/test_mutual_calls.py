# -*- coding: utf-8 -*-
"""
三端互呼测试 — 主入口。

启动方式：
    python test_mutual_calls.py --config config.json
    python test_mutual_calls.py --server 10.0.0.80 --realm nsbc.local --users 1001,1002,1003 --password 876876
    python test_mutual_calls.py --single android,desktop   # 只跑 Android ↔ Desktop

依赖：
    pip install websockets

覆盖场景：
    Android  → Desktop     呼出方向
    Desktop  → Android     呼入方向（同一对账号反过来）
    Android  → iOS
    iOS      → Android
    Desktop  → iOS
    iOS      → Desktop

每对呼叫都做 4 件事：
    1) 双方 REGISTER 成功（200 OK）
    2) SIP 信令序列：INVITE → 200 OK → ACK → BYE → 200 OK
    3) SDP 里 H.264 为 baseline (42e01f + packetization-mode=1)
    4) Opus fmtp 含 maxplaybackrate=8000

退出码：
    0  — 全部 PASS
    1  — 有失败
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import time
import traceback
from dataclasses import dataclass
from typing import Optional

# 把同目录加入 sys.path 以便 import
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from nsbc_harness import (  # noqa: E402
    CallInfo,
    NsbcSipClient,
    fmtp_param,
    parse_sip_message,
)


# ---------------------------------------------------------------------------
# 测试配置
# ---------------------------------------------------------------------------

PLATFORMS = ("android", "desktop", "ios")

PLATFORM_USER_AGENT = {
    "android": "NSBC-Android/1.0",
    "desktop": "NSBC-Desktop/1.0",
    "ios": "NSBC-iOS/1.0",
}


@dataclass
class TestConfig:
    server: str = "10.0.0.80"
    realm: str = "nsbc.local"
    password: str = "876876"
    wss_port: int = 5062
    ws_path: str = "/ws"
    use_tls: bool = True
    allow_self_signed: bool = True
    users: dict[str, str] = None  # platform -> username

    @classmethod
    def from_json(cls, path: str) -> "TestConfig":
        with open(path, encoding="utf-8") as f:
            raw = json.load(f)
        return cls(
            server=raw.get("server", "10.0.0.80"),
            realm=raw.get("realm", "nsbc.local"),
            password=raw.get("password", "876876"),
            wss_port=int(raw.get("wss_port", 5062)),
            ws_path=raw.get("ws_path", "/ws"),
            use_tls=bool(raw.get("use_tls", True)),
            allow_self_signed=bool(raw.get("allow_self_signed", True)),
            users=raw.get("users"),
        )

    @classmethod
    def from_cli(cls, args) -> "TestConfig":
        users_map: dict[str, str] = {}
        if args.users:
            names = [n.strip() for n in args.users.split(",") if n.strip()]
            for i, name in enumerate(names):
                if i < len(PLATFORMS):
                    users_map[PLATFORMS[i]] = name
        else:
            for i, p in enumerate(PLATFORMS):
                offset = args.user_base + i
                users_map[p] = str(offset)
        return cls(
            server=args.server,
            realm=args.realm,
            password=args.password,
            wss_port=int(args.wss_port),
            ws_path=args.ws_path,
            use_tls=bool(args.use_tls),
            allow_self_signed=bool(args.allow_self_signed),
            users=users_map,
        )


# ---------------------------------------------------------------------------
# 断言
# ---------------------------------------------------------------------------


class CheckError(AssertionError):
    pass


def check(cond: bool, msg: str):
    if not cond:
        raise CheckError(msg)


# ---------------------------------------------------------------------------
# SDP 校验
# ---------------------------------------------------------------------------


def verify_h264_baseline(sdp: str, label: str):
    """呼出 SDP 的 H.264 应为 profile-level-id=42e01f 且 packetization-mode=1。"""
    # 找 m=video section 里的 H.264 fmtp 行
    lines = sdp.split("\r\n")
    h264_fmtps = []
    in_video = False
    for line in lines:
        if line.startswith("m=video"):
            in_video = True
            continue
        if in_video and line.startswith("m="):
            break
        if in_video and line.startswith("a=fmtp") and "profile-level-id=" in line:
            h264_fmtps.append(line)

    check(h264_fmtps, f"[{label}] 未找到 H.264 a=fmtp: 行")
    for line in h264_fmtps:
        pid = fmtp_param(line, "profile-level-id")
        check(pid == "42e01f", f"[{label}] H.264 profile-level-id 应为 42e01f，实际 {pid} ({line})")
        pmod = fmtp_param(line, "packetization-mode")
        check(pmod == "1", f"[{label}] H.264 packetization-mode 应为 1，实际 {pmod} ({line})")


def verify_answer_preserves_profile(offer_sdp: str, answer_sdp: str, label: str):
    """呼入 answer 里的 profile-level-id 应与 offer 保持一致（不能被降级）。"""
    offer_lines = [l for l in offer_sdp.split("\r\n") if l.startswith("a=fmtp") and "profile-level-id=" in l]
    answer_lines = [l for l in answer_sdp.split("\r\n") if l.startswith("a=fmtp") and "profile-level-id=" in l]
    if not offer_lines:
        return
    check(answer_lines, f"[{label}] answer 缺失 H.264 fmtp 行")
    offer_pid = fmtp_param(offer_lines[0], "profile-level-id")
    answer_pid = fmtp_param(answer_lines[0], "profile-level-id")
    check(
        answer_pid == offer_pid,
        f"[{label}] answer profile-level-id({answer_pid}) 应与 offer({offer_pid}) 一致",
    )


def verify_opus_maxplaybackrate(sdp: str, label: str):
    """Opus fmtp 应带 maxplaybackrate=8000（三端一致的窄带兼容策略）。"""
    lines = [l for l in sdp.split("\r\n") if l.startswith("a=fmtp:111 ")]
    check(lines, f"[{label}] 未找到 Opus a=fmtp:111 行")
    mpr = fmtp_param(lines[0], "maxplaybackrate")
    check(mpr == "8000", f"[{label}] Opus maxplaybackrate 应为 8000，实际 {mpr} ({lines[0]})")


def verify_sdp_common(sdp: str, label: str):
    """三端共同 SDP 约束。"""
    verify_h264_baseline(sdp, label)
    verify_opus_maxplaybackrate(sdp, label)


# ---------------------------------------------------------------------------
# 单对互呼
# ---------------------------------------------------------------------------


async def run_pair(caller: NsbcSipClient, callee: NsbcSipClient, pair_label: str) -> dict:
    """
    跑完整个互呼 pair：
        - 双方 REGISTER
        - 主叫发 INVITE，被叫自动 200 OK
        - 双方做 SDP 校验
        - 主叫 BYE 挂断
        - 双方 UNREGISTER

    返回 {label, pass, details}。
    """
    details: list[str] = []

    async def _cleanup():
        try:
            await caller.hangup()
        except Exception:
            pass
        try:
            await caller.unregister()
        except Exception:
            pass
        try:
            await callee.unregister()
        except Exception:
            pass
        try:
            await caller.disconnect()
        except Exception:
            pass
        try:
            await callee.disconnect()
        except Exception:
            pass

    try:
        # 1. 双方建连
        await caller.connect()
        await callee.connect()

        # 2. 双方 REGISTER
        ok = await caller.register(expires=300)
        check(ok, f"{pair_label}: REGISTER {caller.username} 失败")
        details.append(f"  ✅ {caller.platform_tag} REGISTER {caller.username}")

        ok = await callee.register(expires=300)
        check(ok, f"{pair_label}: REGISTER {callee.username} 失败")
        details.append(f"  ✅ {callee.platform_tag} REGISTER {callee.username}")

        # 3. 被叫先启动等待（异步监听）
        callee_wait_task = asyncio.create_task(callee.wait_for_incoming(timeout=20.0))

        # 4. 主叫发起呼叫
        target = f"sip:{callee.username}@{caller.realm}"
        call = await caller.call(target, timeout=20.0)
        details.append(f"  ✅ INVITE → {callee.username} 接通 (call_id={call.call_id[:8]})")

        callee_call = await asyncio.wait_for(callee_wait_task, timeout=10.0)
        details.append(f"  ✅ 呼入 {callee.username} 接通 (call_id={callee_call.call_id[:8]})")

        # 5. SDP 校验
        check(call.local_sdp is not None, f"{pair_label}: 主叫本地 offer SDP 缺失")
        verify_sdp_common(call.local_sdp, f"{pair_label} offer({caller.platform_tag})")
        details.append(f"  ✅ offer SDP 校验通过 (H.264 baseline + Opus mpr=8000)")

        check(call.remote_sdp is not None, f"{pair_label}: 对端 answer SDP 缺失")
        verify_answer_preserves_profile(call.local_sdp, call.remote_sdp, f"{pair_label} answer({callee.platform_tag})")
        # answer 只要求保留 profile，不强求一定降级（三端策略是 offer 降级）
        details.append(f"  ✅ answer SDP profile 与 offer 一致")

        # 6. 主叫 BYE 挂断
        await caller.hangup()
        await asyncio.sleep(0.8)  # 给 BYE / 200 OK 留时间
        details.append(f"  ✅ BYE 挂断")

        await _cleanup()
        return {"label": pair_label, "pass": True, "details": details}

    except CheckError as e:
        await _cleanup()
        return {"label": pair_label, "pass": False, "details": details + [f"  ❌ CHECK: {e}"]}
    except Exception as e:
        await _cleanup()
        return {
            "label": pair_label,
            "pass": False,
            "details": details + [f"  ❌ {type(e).__name__}: {e}", traceback.format_exc(limit=3)],
        }


# ---------------------------------------------------------------------------
# 主程序
# ---------------------------------------------------------------------------


async def run_all(cfg: TestConfig, selected: Optional[list[tuple[str, str]]] = None):
    """
    执行全部或部分 pair。
    selected=None 表示全跑（6 对）；否则只跑指定的 (caller_platform, callee_platform) 列表。
    """
    if selected is None:
        selected = [
            ("android", "desktop"),
            ("desktop", "android"),
            ("android", "ios"),
            ("ios", "android"),
            ("desktop", "ios"),
            ("ios", "desktop"),
        ]

    results: list[dict] = []
    t0 = time.time()

    print()
    print("=" * 72)
    print(f"  NSBC 三端互呼测试 — {cfg.server}:{cfg.wss_port}{cfg.ws_path} (tls={cfg.use_tls})")
    print(f"  realm={cfg.realm}")
    for p in PLATFORMS:
        print(f"    {p:8s} → user={cfg.users.get(p)}")
    print("=" * 72)
    print()

    for i, (caller_p, callee_p) in enumerate(selected, 1):
        caller_user = cfg.users.get(caller_p)
        callee_user = cfg.users.get(callee_p)
        if not caller_user or not callee_user:
            print(f"[SKIP] {caller_p}→{callee_p}: 用户未配置")
            continue

        caller = NsbcSipClient(
            server=cfg.server,
            realm=cfg.realm,
            username=caller_user,
            password=cfg.password,
            wss_port=cfg.wss_port,
            ws_path=cfg.ws_path,
            use_tls=cfg.use_tls,
            platform_tag=caller_p,
            allow_self_signed=cfg.allow_self_signed,
        )
        callee = NsbcSipClient(
            server=cfg.server,
            realm=cfg.realm,
            username=callee_user,
            password=cfg.password,
            wss_port=cfg.wss_port,
            ws_path=cfg.ws_path,
            use_tls=cfg.use_tls,
            platform_tag=callee_p,
            allow_self_signed=cfg.allow_self_signed,
        )

        label = f"[{i}/{len(selected)}] {caller_p:8s} ({caller_user}) → {callee_p:8s} ({callee_user})"
        print(f"▶ {label}")
        r = await run_pair(caller, callee, label)
        results.append(r)
        if r["pass"]:
            print(f"  ✅ PASS  ({time.time() - t0:.1f}s elapsed)")
        else:
            print(f"  ❌ FAIL")
            for d in r["details"]:
                print(d)
        print()

    # 汇总
    passed = sum(1 for r in results if r["pass"])
    total = len(results)
    print("=" * 72)
    print(f"  汇总: {passed}/{total} 通过  (总耗时 {time.time() - t0:.1f}s)")
    print("=" * 72)
    for r in results:
        mark = "✅ PASS" if r["pass"] else "❌ FAIL"
        print(f"  {mark}  {r['label']}")

    # 失败的详细输出
    for r in results:
        if not r["pass"]:
            print(f"\n--- FAIL 详情: {r['label']} ---")
            for d in r["details"]:
                print(d)

    return passed == total


def parse_selected(spec: Optional[str]) -> Optional[list[tuple[str, str]]]:
    """--single android,desktop 表示只跑 android→desktop；带逗号对 (a,d),(d,a)。"""
    if not spec:
        return None
    pairs: list[tuple[str, str]] = []
    for token in spec.split(","):
        token = token.strip().lower()
        if "->" in token:
            a, b = token.split("->", 1)
            a = a.strip()
            b = b.strip()
            check(a in PLATFORMS and b in PLATFORMS, f"非法平台对: {token}")
            pairs.append((a, b))
        else:
            # 两个组合都跑
            check(token in PLATFORMS, f"非法平台: {token}")
            for other in PLATFORMS:
                if other != token:
                    pairs.append((token, other))
    # 去重保序
    seen: set[tuple[str, str]] = set()
    out: list[tuple[str, str]] = []
    for p in pairs:
        if p not in seen:
            seen.add(p)
            out.append(p)
    return out


def main():
    ap = argparse.ArgumentParser(
        description="NSBC 三端互呼 SIP 测试脚本",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例：
  # 用配置文件
  python test_mutual_calls.py --config config.example.json

  # 全部默认参数，三账号自动用 1001 / 1002 / 1003
  python test_mutual_calls.py

  # 指定三账号
  python test_mutual_calls.py --users 1001,1002,1003

  # 只跑 Android↔Desktop（两个方向）
  python test_mutual_calls.py --single android,desktop

  # 只跑 Desktop→iOS 单向
  python test_mutual_calls.py --single desktop->ios
""",
    )
    ap.add_argument("--config", help="JSON 配置文件（优先级高于 CLI）")
    ap.add_argument("--server", default="10.0.0.80")
    ap.add_argument("--wss-port", dest="wss_port", type=int, default=5062)
    ap.add_argument("--ws-path", dest="ws_path", default="/ws")
    ap.add_argument("--realm", default="nsbc.local")
    ap.add_argument("--password", default="876876")
    ap.add_argument("--use-tls", dest="use_tls", action="store_true", default=True)
    ap.add_argument("--no-tls", dest="use_tls", action="store_false")
    ap.add_argument("--allow-self-signed", dest="allow_self_signed", action="store_true", default=True)
    ap.add_argument("--users", help="三账号用户名，逗号分隔（顺序：android,desktop,ios）")
    ap.add_argument("--user-base", dest="user_base", type=int, default=1001, help="--users 未指定时的起始编号")
    ap.add_argument("--single", help="只跑部分 pair，格式: android,desktop 或 desktop->ios")

    args = ap.parse_args()

    if args.config:
        cfg = TestConfig.from_json(args.config)
    else:
        cfg = TestConfig.from_cli(args)

    selected = parse_selected(args.single)

    ok = asyncio.run(run_all(cfg, selected))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
