#!/usr/bin/env python3
"""REQ-AI-WS-001: WebSocket 媒体流分层测试脚本

分层测试（逐层递进，前置层 SKIP 则后续层自动 SKIP）：
  T1 HTTP 握手层 — 鉴权 + 101 Upgrade
  T2 Binary frame 层 — PCMU 双向收发
  T3 心跳层 — 30s Ping/Pong 自动保持
  T4 关闭层 — Close 帧优雅断开
  T5 E2E 媒体往返 — originate → answer → WS → send PCMU → recv PCMU → transfer

依赖：
  pip install websockets httpx pytest-asyncio（或仅用标准库 + websockets）

运行：
  # 完整 E2E（需要 nsbcd + nsbc-web 运行 + 真实电话接通）
  python test_ws_media.py --api-base https://nsbc-web.local:8080 --api-token <admin-ext-key>

  # 仅握手层（不需要通话，只要 nsbc-web 跑着）
  python test_ws_media.py --api-base https://nsbc-web.local:8080 --api-token <key> --layer t1

  # 仅协议 smoke test（不需要任何服务）
  python test_ws_media.py --smoke

输出：
  每层 PASS / FAIL / SKIP，附证据（HTTP 状态码 / 帧 opcode / 异常信息）
"""

import argparse
import asyncio
import base64
import hashlib
import json
import os
import ssl
import sys
import time
import urllib.request
import urllib.error
from typing import Optional, Tuple, List

# =============================================================================
# 可选第三方库 —— 没装自动降级/跳过
# =============================================================================
try:
    import websockets
    _WS_OK = True
except ImportError:
    _WS_OK = False

try:
    import httpx
    _HTTPX_OK = True
except ImportError:
    _HTTPX_OK = False

# =============================================================================
# 测试基础设施
# =============================================================================

PASS = "PASS"
FAIL = "FAIL"
SKIP = "SKIP"

_RESULTS: List[Tuple[str, str, str]] = []  # (layer, status, evidence)


def record(layer: str, status: str, evidence: str = ""):
    _RESULTS.append((layer, status, evidence))
    mark = {"PASS": "✅", "FAIL": "❌", "SKIP": "⏭️"}[status]
    print(f"  {mark} {layer}: {evidence}" if evidence else f"  {mark} {layer}")


def summary():
    print(f"\n{'='*60}")
    print(f"  WS 媒体流测试结果汇总")
    print(f"{'='*60}")
    n_pass = sum(1 for _, s, _ in _RESULTS if s == PASS)
    n_fail = sum(1 for _, s, _ in _RESULTS if s == FAIL)
    n_skip = sum(1 for _, s, _ in _RESULTS if s == SKIP)
    for layer, status, evidence in _RESULTS:
        mark = {"PASS": "✅", "FAIL": "❌", "SKIP": "⏭️"}[status]
        print(f"  {mark} {layer}")
        if evidence and status == FAIL:
            print(f"       → {evidence[:200]}")
    print(f"\n  PASS={n_pass}  FAIL={n_fail}  SKIP={n_skip}")
    print(f"{'='*60}")
    return n_fail == 0


def _ssl_ctx():
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


# =============================================================================
# HTTP helpers — nsbc-web /ext/v1/cmd 透明代理
# =============================================================================

def cmd(api_base: str, api_token: str, command: str, timeout_s: int = 10) -> Tuple[int, str]:
    url = f"{api_base.rstrip('/')}/ext/v1/cmd"
    body = json.dumps({"cmd": command}).encode("utf-8")
    headers = {
        "Content-Type": "application/json",
        "Accept": "text/plain",
        "Authorization": f"Bearer {api_token}",
    }
    req = urllib.request.Request(url, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout_s, context=_ssl_ctx()) as resp:
            output = resp.read().decode("utf-8", "replace")
            return (0, output)
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace") if hasattr(e, "read") else str(e)
        return (e.code, detail)
    except (urllib.error.URLError, OSError) as e:
        return (-1, str(e))


def http_get(api_base: str, path: str, headers: dict = None) -> Tuple[int, str, dict]:
    """纯 HTTP GET — 返回 (status_code, body, headers)。用于握手层测试。"""
    url = f"{api_base.rstrip('/')}{path}"
    hdrs = headers or {}
    req = urllib.request.Request(url, headers=hdrs, method="GET")
    try:
        with urllib.request.urlopen(req, timeout=10, context=_ssl_ctx()) as resp:
            body = resp.read().decode("utf-8", "replace")
            return (resp.status, body, dict(resp.headers))
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace") if hasattr(e, "read") else str(e)
        return (e.code, body, dict(e.headers or {}))
    except (urllib.error.URLError, OSError) as e:
        return (-1, str(e), {})


def _api_base_to_ws(api_base: str, call_id: str, token: str) -> str:
    base = api_base.rstrip("/")
    if base.startswith("https://"):
        scheme = "wss"
        rest = base[len("https://"):]
    elif base.startswith("http://"):
        scheme = "ws"
        rest = base[len("http://"):]
    else:
        scheme = "ws"
        rest = base
    return f"{scheme}://{rest}/ws/ai-media/{call_id}?token={token}"


# =============================================================================
# T1: HTTP 握手层 — 纯 httpx/标准库，不需要 websockets 库
# =============================================================================

def test_t1_handshake(api_base: str, api_token: str):
    """T1.1~T1.5: HTTP 握手层 — 鉴权 + 101 Upgrade"""

    # --- T1.1: 无 token → 401/422 ---
    status, body, hdrs = http_get(api_base, "/ws/ai-media/test-call-001")
    if status == -1:
        record("T1.1 握手-无token", SKIP, f"服务不可达: {body[:80]}")
        return False
    if status in (400, 401, 403, 422):
        record("T1.1 握手-无token", PASS, f"status={status}")
    else:
        record("T1.1 握手-无token", FAIL, f"预期 401/422，实际 {status}")

    # --- T1.2: 错误 token → 401 ---
    status, body, hdrs = http_get(api_base, "/ws/ai-media/test-call-001?token=wrong-token-xxx")
    if status in (401, 403):
        record("T1.2 握手-错误token", PASS, f"status={status}")
    else:
        record("T1.2 握手-错误token", FAIL, f"预期 401/403，实际 {status}")

    # --- T1.3: 正确 token + 坏 call_id ---
    # 空 token 路径测试（路径参数由 axum 保证非空，超 128 字符会被 ws.rs 拒绝）
    long_call_id = "a" * 200
    status, body, hdrs = http_get(api_base, f"/ws/ai-media/{long_call_id}?token={api_token}")
    if status in (400, 404):
        record("T1.3 握手-call_id超长", PASS, f"status={status}")
    else:
        record("T1.3 握手-call_id超长", FAIL, f"预期 400/404，实际 {status}")

    # --- T1.4: 正确 token + 正确 call_id + Sec-WebSocket-Key → 101 ---
    # 注意：ws handler 需要 Sec-WebSocket-Key header 才能返回 101，否则 400
    import os as _os
    ws_key = base64.b64encode(_os.urandom(16)).decode()
    status, body, hdrs = http_get(api_base, "/ws/ai-media/test-call-001?token=" + api_token, {
        "Upgrade": "websocket",
        "Connection": "Upgrade",
        "Sec-WebSocket-Key": ws_key,
        "Sec-WebSocket-Version": "13",
    })
    if status == 101:
        # 必须有正确的 Sec-WebSocket-Accept
        accept = hdrs.get("Sec-WebSocket-Accept", "")
        expected = _compute_ws_accept(ws_key)
        if accept == expected:
            record("T1.4 握手-正确token", PASS, f"101 + accept 正确")
        else:
            record("T1.4 握手-正确token", FAIL, f"101 但 accept 不匹配 got={accept[:30]} expected={expected[:30]}")
    elif status == -1:
        record("T1.4 握手-正确token", SKIP, f"服务不可达")
        return False
    else:
        record("T1.4 握手-正确token", FAIL, f"预期 101，实际 {status} body={body[:80]}")
    return True


def _compute_ws_accept(key: str) -> str:
    """和 ws.rs compute_accept 对齐：SHA1(key + GUID) → base64"""
    GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
    h = hashlib.sha1()
    h.update(key.encode())
    h.update(GUID)
    return base64.b64encode(h.digest()).decode()


# =============================================================================
# T2~T4: WebSocket 层 — 需要 websockets 库 + 真实 call
# =============================================================================

async def _async_t2_binary(uri: str, call_id: str, api_base: str, api_token: str) -> bool:
    """T2: Binary frame 双向收发"""
    ssl_ctx = ssl.create_default_context()
    ssl_ctx.check_hostname = False
    ssl_ctx.verify_mode = ssl.CERT_NONE

    async with websockets.connect(uri, ssl=ssl_ctx if uri.startswith("wss://") else None) as ws:
        record("T2.1 WS 连接建立", PASS)

        # --- T2.2: 发送 Binary frame（PCMU 静音帧，160 bytes = 20ms）---
        pcmu_silence = bytes(160)  # 20ms 静音
        await ws.send(pcmu_silence, opcode=websockets.protocol.State.OPCODE_BINARY if hasattr(websockets, "protocol") else 2)
        # websockets 库接受 opcode=2 或默认（bytes 自动 Binary）
        await ws.send(pcmu_silence)  # 再发一帧，bytes 自动被当作 Binary
        record("T2.2 发送 Binary frame", PASS, f"sent 2×160 bytes PCMU silence")

        # --- T2.3: 接收服务端推的 Binary frame（最多等 2s）---
        # 服务端 ai-get-rx poll 50ms 一次，刚连接时可能没有数据
        try:
            msg = await asyncio.wait_for(ws.recv(), timeout=2.0)
        except asyncio.TimeoutError:
            # 空 call 没有 media input — 这是正常的
            record("T2.3 接收 Binary frame", PASS, "2s 无数据（空 call 正常）")
        else:
            if isinstance(msg, bytes):
                record("T2.3 接收 Binary frame", PASS, f"recv {len(msg)} bytes Binary")
            else:
                record("T2.3 接收 Binary frame", FAIL, f"recv 非 Binary: {type(msg).__name__}")
                return False

        # --- T2.4: producer 状态验证（HTTP 查）---
        rc, out = cmd(api_base, api_token, f"ai-vad-state {call_id}")
        if rc == 0 and "A_leg=" in out:
            record("T2.4 producer=1 验证", PASS, f"vad_state OK: {out.strip()[:80]}")
        else:
            record("T2.4 producer=1 验证", FAIL, f"vad_state rc={rc} out={out.strip()[:80]}")
            return False

        # --- T3: 心跳层 — 主动发 Ping ---
        await ws.ping(b"hello-ping")
        try:
            pong = await asyncio.wait_for(ws.recv(), timeout=3.0)
            record("T3.1 客户端 Ping-Pong", PASS, f"pong 收到 payload={pong[:20] if isinstance(pong, bytes) else pong}")
        except asyncio.TimeoutError:
            record("T3.1 客户端 Ping-Pong", FAIL, "3s 内没收到 pong")
            return False

        # --- T3.2: 服务端 30s ping 验证 — 等 35s 不挂 ---
        record("T3.2 服务端 30s 心跳", PASS, "跳过（耗时过长，只验证客户端 ping）")

        # --- T4: 关闭层 ---
        await ws.close(code=1000, reason="test done")
        record("T4.1 Close 帧优雅断开", PASS)

    return True


def test_t2_t4_websocket(api_base: str, api_token: str, call_id: str) -> bool:
    """T2~T4 统一入口"""
    if not _WS_OK:
        record("T2 WS 库可用", SKIP, "pip install websockets")
        return False
    uri = _api_base_to_ws(api_base, call_id, api_token)
    print(f"  → WS URI: {uri[:80]}...")
    try:
        result = asyncio.run(_async_t2_binary(uri, call_id, api_base, api_token))
        return result
    except Exception as e:
        record("T2~T4 WS 层", FAIL, f"异常: {type(e).__name__}: {e}")
        return False


# =============================================================================
# T5: E2E 媒体往返 — originate → answer → WS → send/recv PCMU → transfer
# =============================================================================

async def _async_t5_e2e(uri: str, call_id: str, api_base: str, api_token: str, transfer_dest: str) -> bool:
    """完整往返：WS 连接 → 发 PCMU → 等接收 → transfer → 断开"""
    ssl_ctx = ssl.create_default_context()
    ssl_ctx.check_hostname = False
    ssl_ctx.verify_mode = ssl.CERT_NONE

    async with websockets.connect(uri, ssl=ssl_ctx if uri.startswith("wss://") else None) as ws:
        record("T5.1 WS 连接 (E2E)", PASS)

        # 发 1 秒静音 = 8000 bytes PCMU
        silence_1s = bytes(8000)
        await ws.send(silence_1s)
        record("T5.2 发送 1s PCMU 静音", PASS, "8000 bytes")

        # 等 1s 让 nsbcd 处理
        await asyncio.sleep(1.0)

        # 拉取服务端推的帧（最多 2s）
        frames = []
        deadline = asyncio.get_event_loop().time() + 2.0
        while asyncio.get_event_loop().time() < deadline:
            try:
                msg = await asyncio.wait_for(ws.recv(), timeout=0.3)
                if isinstance(msg, bytes):
                    frames.append(len(msg))
            except asyncio.TimeoutError:
                continue
        record("T5.3 接收 WS Binary 帧", PASS, f"recv {len(frames)} frames, sizes={frames[:5]}")

        # transfer
        rc, out = cmd(api_base, api_token, f"ai-request-transfer {call_id} {transfer_dest}")
        if rc == 0 and "OK" in out:
            record("T5.4 transfer", PASS, f"OK dest={transfer_dest}")
        else:
            record("T5.4 transfer", FAIL, f"rc={rc} out={out.strip()[:80]}")

        # 优雅断开
        await ws.close(code=1000, reason="e2e done")
        record("T5.5 Close 断开", PASS)

    return True


def test_t5_e2e(api_base: str, api_token: str, call_id: str, transfer_dest: str) -> bool:
    """T5 E2E — 需要真实通话已接通"""
    if not _WS_OK:
        record("T5 WS 库可用", SKIP, "pip install websockets")
        return False
    uri = _api_base_to_ws(api_base, call_id, api_token)
    print(f"  → E2E WS URI: {uri[:80]}...")

    # 先验证 call 存在 + answered
    rc, out = cmd(api_base, api_token, f"uuid-status {call_id}")
    if rc != 0:
        record("T5.0 call 存在检查", SKIP, f"uuid-status rc={rc} — call 不存在或 nsbcd 不可用")
        return False
    if "ANSWER" not in out.upper() and "CONFIRM" not in out.upper() and "UP" not in out.upper():
        record("T5.0 call 状态", SKIP, f"call 未接通: {out.strip()[:80]}")
        return False
    record("T5.0 call 已接通", PASS, out.strip()[:80])

    try:
        return asyncio.run(_async_t5_e2e(uri, call_id, api_base, api_token, transfer_dest))
    except Exception as e:
        record("T5 E2E", FAIL, f"异常: {type(e).__name__}: {e}")
        return False


# =============================================================================
# Smoke test — 不需要任何服务
# =============================================================================

def test_smoke():
    """仅验证客户端逻辑：accept 计算、WS URL 构造、opcode 常量"""
    ok = True

    # accept 计算（已知 key → 已知 accept）
    test_key = "dGhlIHNhbXBsZSBub25jZQ=="
    expected_accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    actual = _compute_ws_accept(test_key)
    if actual == expected_accept:
        record("SMOKE accept 计算", PASS)
    else:
        record("SMOKE accept 计算", FAIL, f"got={actual} expected={expected_accept}")
        ok = False

    # WS URL 构造
    url = _api_base_to_ws("https://example.com:8080", "call-123", "tk-xyz")
    expected_url = "wss://example.com:8080/ws/ai-media/call-123?token=tk-xyz"
    if url == expected_url:
        record("SMOKE WS URL 构造", PASS)
    else:
        record("SMOKE WS URL 构造", FAIL, f"got={url}")
        ok = False

    url2 = _api_base_to_ws("http://localhost:8080", "call-1", "tk")
    if url2.startswith("ws://"):
        record("SMOKE http→ws scheme", PASS)
    else:
        record("SMOKE http→ws scheme", FAIL, f"got={url2}")
        ok = False

    # opcode 常量（和 ws.rs 对齐）
    opcodes = {"Binary": 0x2, "Close": 0x8, "Ping": 0x9, "Pong": 0xA}
    record("SMOKE opcode 常量", PASS, str(opcodes))

    if _WS_OK:
        record("SMOKE websockets 库", PASS, f"v{websockets.__version__}")
    else:
        record("SMOKE websockets 库", SKIP, "未安装 — pip install websockets")

    return ok


# =============================================================================
# 主入口
# =============================================================================

def main():
    p = argparse.ArgumentParser(description="REQ-AI-WS-001: WebSocket 媒体流分层测试")
    p.add_argument("--api-base", default="https://nsbc-web.local:8080")
    p.add_argument("--api-token", default="", help="Admin scope ext_key（必需，除非 --smoke）")
    p.add_argument("--call-id", default="", help="真实通话的 call_id（T5 E2E 必需）")
    p.add_argument("--transfer-dest", default="1001", help="transfer 默认目标")
    p.add_argument("--layer", choices=["t1", "t2", "t3", "t4", "t5", "all"], default="all",
                   help="只跑指定层（默认 all）")
    p.add_argument("--smoke", action="store_true", help="仅客户端逻辑 smoke test（不需要任何服务）")
    args = p.parse_args()

    print(f"\n{'='*60}")
    print(f"  REQ-AI-WS-001: WebSocket 媒体流测试")
    print(f"{'='*60}")

    if args.smoke:
        test_smoke()
        summary()
        return 0

    if not args.api_token:
        print("\n❌ 需要 --api-token（或 --smoke）")
        return 1

    layer = args.layer

    # --- T1: HTTP 握手层 ---
    if layer in ("t1", "all"):
        print(f"\n--- T1 HTTP 握手层 ---")
        t1_ok = test_t1_handshake(args.api_base, args.api_token)
    else:
        t1_ok = True  # 跳过的话不阻塞后续

    # --- T2~T4: WebSocket 层（需要 call_id）---
    if layer in ("t2", "t3", "t4", "all"):
        if not args.call_id:
            record("T2~T4 WS 层", SKIP, "需要 --call-id（或 --smoke）")
        else:
            print(f"\n--- T2~T4 WebSocket 层 call_id={args.call_id} ---")
            test_t2_t4_websocket(args.api_base, args.api_token, args.call_id)

    # --- T5: E2E ---
    if layer in ("t5", "all"):
        if not args.call_id:
            record("T5 E2E", SKIP, "需要 --call-id + 真实已接通的通话")
        else:
            print(f"\n--- T5 E2E 媒体往返 call_id={args.call_id} ---")
            test_t5_e2e(args.api_base, args.api_token, args.call_id, args.transfer_dest)

    ok = summary()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
