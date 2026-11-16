#!/usr/bin/env python3
"""REQ-AI-MEDIA-001 + REQ-AI-BARGEIN-001 + REQ-AI-WS-001: External AI Agent

架构：
  ┌─────────────┐  SSE events  ┌──────────────┐  /ext/v1/cmd   ┌─────────────┐
  │  nsbc-web   │ ───────────→ │  ai_agent.py  │ ───────────────→ │   nsbcd     │
  │  (Rust)     │ ←─────────── │  (本文件)     │ ←─────────────── │ (C 核心)    │
  └─────────────┘  tx_audio    └──────────────┘  ai-send-tx      └─────────────┘
                                              │
                                              ├── [HTTP] ai-get-rx        ← 用户语音 (PCMU 8kHz base64)
                                              ├── [HTTP] ai-vad-state      ← VAD barge-in (SPEECH/SILENCE + RMS)
                                              ├── [HTTP] ai-set-producer  ← 自动启 VAD
                                              ├── [HTTP] ai-request-transfer → blind REFER 转人工座席
                                              │
                                              └── [WS]   /ws/ai-media/:call_id  ← Binary frame 零开销媒体流

transport 选择：
  --transport http   默认：HTTP polling + base64（零第三方依赖）
  --transport ws     WebSocket Binary frame（pip install websockets，零 base64 开销）
  --transport auto   尝试 WS，websockets 未装自动降级 HTTP

本文件演示完整的外部 AI agent 骨架：
  - producer 模式接管 + VAD 自动启动（barge-in ready）
  - barge-in：VAD 检测到用户说话 → 暂停 TTS → 等用户说完 → 继续对话
  - LLM function call：返回 {"action":"transfer","dest":"1001"} → 自动转人工座席
  - LLM function call：返回 {"action":"hangup"} → 自动挂断
  - ai-send-tx 分片：> 3KB 自动切成多个 chunk 发送（命令行 4KB 限制）
  - mock ASR/LLM/TTS：生产环境替换成真服务
  - WebSocket transport：Binary frame 直传 PCMU，零 base64 开销

命令：
  python3 ai_agent.py --api-base https://nsbc-web.local:8080 \\
      --api-token <Admin-ext-key> \\
      --call-id <从 SSE CALL_ANSWERED 事件拿到的 call_id> \\
      --transfer-dest 1001 \\
      --transport auto             # http | ws | auto（默认 auto）\\
      --vad-barge-in                # 启用 barge-in（默认开）\\
      --barge-in-rms 800            # barge-in RMS 阈值（默认 800，和 VAD 默认对齐）\\
      --chunk-bytes 160             # 每次 get_rx 拉多少字节（默认 160 = 20ms PCMU）
"""

import argparse
import base64
import json
import time
import urllib.request
import urllib.error
import ssl
import sys
from typing import Optional, Tuple, List

# REQ-AI-WS-001: websockets 可选导入，没装自动降级 HTTP
try:
    import websockets
    _WS_AVAILABLE = True
except ImportError:
    _WS_AVAILABLE = False

# =============================================================================
# HTTP helpers — 调 nsbc-web /ext/v1/cmd 透明代理（Admin scope API key）
# =============================================================================

def _ssl_ctx():
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


def cmd(api_base: str, api_token: str, command: str, timeout_s: int = 10) -> Tuple[int, str]:
    """POST /ext/v1/cmd —— 透明代理 nsbcd OAM 命令。
    返回 (http_rc, output_text)。http_rc=0 表示成功（output 里有 'OK'）。"""
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


def _check_ok(rc: int, output: str) -> bool:
    return rc == 0 and "OK" in output and "ERR" not in output


# =============================================================================
# AI pass-through API（REQ-AI-MEDIA-001 + REQ-AI-BARGEIN-001）
# =============================================================================

MAX_CMD_LINE = 4096   # UDS 命令行实际极限（保守值，base64 输入后 ~2700 字节 PCMU 原始）

def ai_set_producer(api_base: str, api_token: str, call_id: str, producer: int = 1) -> bool:
    rc, out = cmd(api_base, api_token, f"ai-set-producer {call_id} {producer}")
    ok = _check_ok(rc, out)
    print(f"[ai] set_producer call={call_id} producer={producer} → {out.strip()[:150]}")
    return ok


def ai_vad_state(api_base: str, api_token: str, call_id: str) -> Tuple[str, int, str, int]:
    """REQ-AI-BARGEIN-001: 查询 VAD 状态。
    返回 (a_state, a_rms, b_state, b_rms) —— state='SPEECH'|'SILENCE'，rms 整数。
    VAD 未激活时返回 ('SILENCE', 0, 'SILENCE', 0)。"""
    rc, out = cmd(api_base, api_token, f"ai-vad-state {call_id}")
    a_state, a_rms = "SILENCE", 0
    b_state, b_rms = "SILENCE", 0
    for line in out.split("\n"):
        line = line.strip()
        if line.startswith("OK A_leg="):
            parts = line.replace("OK A_leg=", "").split()
            a_state = parts[0]
            a_rms = int(parts[1].replace("rms=", "")) if len(parts) > 1 else 0
        elif line.startswith("OK B_leg="):
            parts = line.replace("OK B_leg=", "").split()
            b_state = parts[0]
            b_rms = int(parts[1].replace("rms=", "")) if len(parts) > 1 else 0
    return a_state, a_rms, b_state, b_rms


def ai_request_transfer(api_base: str, api_token: str, call_id: str, dest: str) -> bool:
    rc, out = cmd(api_base, api_token, f"ai-request-transfer {call_id} {dest}")
    ok = _check_ok(rc, out)
    print(f"[ai] ★ request_transfer call={call_id} dest={dest} → {out.strip()[:120]}")
    return ok


def ai_hangup_via_uuid_kill(api_base: str, api_token: str, call_id: str) -> bool:
    """LLM function call {"action":"hangup"} 走 uuid-kill。"""
    rc, out = cmd(api_base, api_token, f"uuid-kill {call_id}")
    ok = _check_ok(rc, out)
    print(f"[ai] ★ hangup call={call_id} → {out.strip()[:120]}")
    return ok


def ai_get_rx_audio(api_base: str, api_token: str, call_id: str, max_bytes: int = 160) -> bytes:
    """拉用户语音（PCMU 8kHz base64）。非阻塞，空返回 b''。"""
    rc, out = cmd(api_base, api_token, f"ai-get-rx {call_id} {max_bytes}")
    if rc != 0:
        return b""
    for line in out.split("\n"):
        if line.startswith("b64:"):
            try:
                return base64.b64decode(line[4:].strip())
            except Exception:
                return b""
    return b""


def ai_send_tx_audio(api_base: str, api_token: str, call_id: str,
                     pcm_bytes: bytes, format_pcm16: bool = False) -> bool:
    """推回复音频。> 3KB 自动分片（命令行 4KB 限制）。"""
    if not pcm_bytes:
        return True
    fmt = 1 if format_pcm16 else 0
    # 分片：每 chunk ≤ MAX_CMD_LINE - 80（留命令头 + 开销）
    chunk_cap = (MAX_CMD_LINE - 80) * 3 // 4  # base64 膨胀 4/3
    for i in range(0, len(pcm_bytes), chunk_cap):
        chunk = pcm_bytes[i:i+chunk_cap]
        b64 = base64.b64encode(chunk).decode("ascii")
        rc, out = cmd(api_base, api_token, f"ai-send-tx {call_id} {fmt} {b64}")
        if not _check_ok(rc, out):
            print(f"[ai] send_tx FAILED chunk={i} rc={rc} out={out.strip()[:100]}", file=sys.stderr)
            return False
    return True


# =============================================================================
# Mock AI pipeline — 生产环境替换成真服务
# =============================================================================

KEYWORD_TRANSFER = ["转人工", "人工", "客服", "人工服务", "找人工", "转客服", "人工台"]
KEYWORD_TRANSFER_EN = ["agent", "human", "operator", "representative"]
KEYWORD_HANGUP = ["挂断", "再见", "拜拜", "谢谢", "hangup", "bye"]


def mock_asr(pcmu_bytes: bytes) -> str:
    """Mock ASR：检测 PCMU 是否有声音（能量），有就返回固定词。
    生产环境：Whisper / 阿里云 / 腾讯云 ASR（audio=base64, lang='zh'）。"""
    if not pcmu_bytes:
        return ""
    if any(b > 24 for b in pcmu_bytes):
        return "我想转人工"   # 测试关键词触发 transfer
    return ""


def mock_llm(transcript: str) -> str:
    """Mock LLM：返回 **structured JSON**（REQ-AI-MEDIA-001 function call 格式）。
    生产环境：Chat Completions / Ollama，要求 JSON mode。

    action 字段：
      "say"       → 文本回复 → mock_tts → ai_send_tx_audio
      "transfer"  → ai_request_transfer(dest)
      "hangup"    → ai_hangup_via_uuid_kill()
      "silence"   → 不回复（用户没说话 / barge-in 中）"""
    if not transcript:
        return json.dumps({"action": "silence"})

    lowered = transcript.lower()
    # function call：transfer
    for kw in KEYWORD_TRANSFER + KEYWORD_TRANSFER_EN:
        if kw in lowered:
            return json.dumps({"action": "transfer", "dest": "1001", "reason": f"keyword={kw}"})
    # function call：hangup
    for kw in KEYWORD_HANGUP:
        if kw in lowered:
            return json.dumps({"action": "hangup", "reason": f"keyword={kw}"})

    # 默认 say
    reply = f"我收到了您说的：{transcript[:40]}。请问还需要什么帮助吗？"
    return json.dumps({"action": "say", "text": reply})


def mock_tts(text: str) -> bytes:
    """Mock TTS：返回静音帧（0x00 的 PCMU 8kHz）。
    生产环境：ElevenLabs / VITS / ChatTTS → 返回 PCMU 或 PCM16。"""
    if not text:
        return b""
    # 2 秒静音 = 16000 字节 PCMU（8k * 2s）。替换成真 TTS 后会返回真实语音。
    return bytes(16000)


# =============================================================================
# REQ-AI-WS-001: WebSocket 媒体流 transport（Binary frame 直传 PCMU，零 base64 开销）
# =============================================================================

def _api_base_to_ws(api_base: str, call_id: str, api_token: str) -> str:
    """https://host:8080 → wss://host:8080/ws/ai-media/{call_id}?token={api_token}"""
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
    return f"{scheme}://{rest}/ws/ai-media/{call_id}?token={api_token}"


class WsMediaTransport:
    """REQ-AI-WS-001: WebSocket Binary frame 媒体流 transport。

    替代 ai_get_rx_audio / ai_send_tx_audio（HTTP polling + base64），
    用 /ws/ai-media/:call_id Binary frame 直传 PCMU 8kHz 原始字节。

    设计：
      - 一个长连接，asyncio 跑 loop（recv/send），主线程取数据走 queues
      - producer=1 由 WS handler 自动开启（连接建立时 nsbc-web 调 ai-set-producer 1）
      - producer=0 由 WS handler 自动关闭（Close 帧），finally 里手动再调一次保持幂等
      - barge-in / transfer / hangup 继续走 HTTP（这些是控制命令，不走媒体流）
    """

    def __init__(self, api_base: str, api_token: str, call_id: str):
        self.api_base = api_base
        self.api_token = api_token
        self.call_id = call_id
        self.uri = _api_base_to_ws(api_base, call_id, api_token)
        self._ws = None
        self._loop = None
        self._rx_bytequeue: List[bytes] = []   # 收到的 Binary frame (PCMU bytes)
        self._rx_index = 0                      # 当前 frame 消费到哪
        self._connected = False
        self._lock = None

    # --- 同步 API（和 HTTP transport 签名对齐）---

    def connect(self, timeout_s: float = 10.0) -> bool:
        """建立 WS 连接。成功返回 True。"""
        if not _WS_AVAILABLE:
            print("[ws] websockets 库未安装，请 pip install websockets", file=sys.stderr)
            return False
        import asyncio
        try:
            self._loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self._loop)
            self._loop.run_until_complete(self._async_connect(timeout_s))
            return self._connected
        except Exception as e:
            print(f"[ws] connect 失败: {e}", file=sys.stderr)
            self._connected = False
            return False

    def get_rx(self, max_bytes: int = 160) -> bytes:
        """拉用户语音（PCMU 8kHz 原始字节，零 base64 开销）。非阻塞，空返回 b''。"""
        if not self._connected:
            return b""
        import asyncio
        try:
            return self._loop.run_until_complete(self._async_get_rx(max_bytes))
        except Exception:
            return b""

    def send_tx(self, pcmu_bytes: bytes) -> bool:
        """推回复音频（PCMU 8kHz 原始字节）。WS Binary frame 不分片（TCP 自动处理）。"""
        if not self._connected or not pcmu_bytes:
            return True
        import asyncio
        try:
            self._loop.run_until_complete(self._async_send_tx(pcmu_bytes))
            return True
        except Exception as e:
            print(f"[ws] send_tx err: {e}", file=sys.stderr)
            self._connected = False
            return False

    def close(self) -> None:
        """优雅断开（WS handler 自动 producer=0）。"""
        if self._loop is None or self._ws is None:
            return
        import asyncio
        try:
            self._loop.run_until_complete(self._async_close())
        except Exception:
            pass
        self._connected = False
        try:
            self._loop.close()
        except Exception:
            pass
        self._loop = None

    # --- asyncio 内部实现 ---

    async def _async_connect(self, timeout_s: float):
        ssl_ctx = ssl.create_default_context()
        ssl_ctx.check_hostname = False
        ssl_ctx.verify_mode = ssl.CERT_NONE
        try:
            self._ws = await asyncio.wait_for(
                websockets.connect(self.uri, ssl=ssl_ctx if self.uri.startswith("wss://") else None),
                timeout=timeout_s,
            )
            self._connected = True
            print(f"[ws] 已连接 {self.uri[:80]}...  → handler 自动 producer=1")
        except asyncio.TimeoutError:
            self._connected = False
            raise

    async def _async_get_rx(self, max_bytes: int) -> bytes:
        """非阻塞拉取：先消费 buffer，buffer 空就 asyncio.wait_for(recv, timeout=50ms)。"""
        # 先消费已有 buffer
        if self._rx_bytequeue:
            frame = self._rx_bytequeue[0]
            remaining = len(frame) - self._rx_index
            if remaining <= max_bytes:
                self._rx_index = 0
                self._rx_bytequeue.pop(0)
                return frame[self._rx_index:]
            chunk = frame[self._rx_index:self._rx_index + max_bytes]
            self._rx_index += max_bytes
            return chunk

        # buffer 空 — 尝试 recv（50ms timeout，对齐 HTTP polling 节奏）
        try:
            msg = await asyncio.wait_for(self._ws.recv(), timeout=0.05)
        except asyncio.TimeoutError:
            return b""
        except Exception as e:
            print(f"[ws] recv err: {e}", file=sys.stderr)
            self._connected = False
            return b""

        # Binary frame → 直接返回（handler 会切分，或我们自己缓存）
        if isinstance(msg, bytes):
            # 如果这个 frame 太大（一次超过 max_bytes），缓存起来
            if len(msg) > max_bytes:
                self._rx_bytequeue.append(msg)
                self._rx_index = max_bytes
                return msg[:max_bytes]
            return msg

        # Text frame 忽略（handler 只发 Binary）
        return b""

    async def _async_send_tx(self, pcmu_bytes: bytes):
        await self._ws.send(pcmu_bytes)

    async def _async_close(self):
        try:
            await self._ws.close(code=1000, reason="agent shutdown")
        except Exception:
            pass


# =============================================================================
# barge-in：AI 正在说话时用户打断 → 暂停 TTS → 等用户说完
# =============================================================================

class BargeInController:
    """REQ-AI-BARGEIN-001: VAD-based barge-in 控制器。

    逻辑：
      1. AI 正在说话（tx_audio 循环中）时，并行 ai_vad_state 查询 A-leg（用户侧）
      2. VAD_STATE_SPEECH 持续出现 → 用户在打断 → 立即停 ai_send_tx
      3. AI 说话前查一次 VAD → 用户在说就 defer（等用户说完）"""

    def __init__(self, api_base, api_token, call_id, rms_threshold=800):
        self.api_base = api_base
        self.api_token = api_token
        self.call_id = call_id
        self.rms_threshold = rms_threshold
        self.barge_in_triggered = False
        self.barge_in_count = 0

    def check_user_speaking(self) -> bool:
        """查 VAD A-leg（用户侧）是否在说话。"""
        a_state, a_rms, b_state, b_rms = ai_vad_state(self.api_base, self.api_token, self.call_id)
        return a_state == "SPEECH" and a_rms >= self.rms_threshold

    def wait_until_silence(self, timeout_s: float = 2.0, poll_interval_s: float = 0.05) -> bool:
        """等用户说完（连续 N 次 SILENCE），超时返回 False。"""
        consecutive_silence = 0
        required = 10   # 10 * 50ms = 500ms 连续静默才认为说完
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if self.check_user_speaking():
                consecutive_silence = 0
            else:
                consecutive_silence += 1
                if consecutive_silence >= required:
                    return True
            time.sleep(poll_interval_s)
        return False


# =============================================================================
# 外部 AI agent 主循环
# =============================================================================

def ai_loop(api_base: str, api_token: str, call_id: str,
            transfer_dest: str = "1001",
            barge_in_enabled: bool = True,
            barge_in_rms: int = 800,
            chunk_bytes: int = 160,
            transport: str = "auto") -> None:

    # --- transport 选择 ---
    use_ws = False
    ws_transport = None
    transport_label = "HTTP polling"
    if transport == "ws" or (transport == "auto" and _WS_AVAILABLE):
        ws_transport = WsMediaTransport(api_base, api_token, call_id)
        if ws_transport.connect(timeout_s=10.0):
            use_ws = True
            transport_label = "WebSocket Binary frame"
        else:
            if transport == "ws":
                print("[ai_agent] 指定 --transport ws 但连接失败，退出", file=sys.stderr)
                return
            print("[ai_agent] WS 连接失败，降级 HTTP polling")

    print(f"\n{'='*70}")
    print(f"[ai_agent] 接管 call={call_id}  transfer_dest={transfer_dest}")
    print(f"  barge_in={'on' if barge_in_enabled else 'off'}  chunk_bytes={chunk_bytes}")
    print(f"  transport={transport_label}")
    print(f"{'='*70}\n")

    # producer=1：WS transport 由 handler 自动开，HTTP transport 手动调
    if use_ws:
        print("[ai_agent] WS handler 已自动 producer=1 → VAD on")
    else:
        if not ai_set_producer(api_base, api_token, call_id, producer=1):
            print("[ai_agent] producer 设置失败，退出", file=sys.stderr)
            return

    barge = BargeInController(api_base, api_token, call_id, barge_in_rms) if barge_in_enabled else None

    # --- rx/tx helper（transport 抽象）---
    def _get_rx() -> bytes:
        if use_ws:
            return ws_transport.get_rx(max_bytes=chunk_bytes)
        return ai_get_rx_audio(api_base, api_token, call_id, max_bytes=chunk_bytes)

    def _send_tx(pcmu: bytes) -> bool:
        if use_ws:
            return ws_transport.send_tx(pcmu)
        return ai_send_tx_audio(api_base, api_token, call_id, pcmu)

    # 先发一条欢迎语
    welcome = mock_tts("您好，我是 AI 助理。请问需要什么帮助？")
    if welcome:
        print(f"[ai_agent] → TTS 欢迎语 ({len(welcome)} bytes)")
        _send_tx(welcome)

    print("[ai_agent] 循环开始 — 输入 'quit' 退出\n")

    try:
        while True:
            # --- Step 2: 拉用户语音 ---
            rx = _get_rx()
            if not rx:
                time.sleep(0.05)

                # stdin 手动输入（测试用）
                import select as _sel
                if sys.stdin in _sel.select([sys.stdin], [], [], 0)[0]:
                    line = sys.stdin.readline().strip()
                    if line.lower() == "quit":
                        print("[ai_agent] 手动退出")
                        break
                    if line.startswith("tx "):
                        reply = mock_tts(line[3:])
                        if reply:
                            _send_tx(reply)
                continue

            # --- Step 3: 如果 barge-in 开着，先等用户说完 ---
            if barge:
                barge.wait_until_silence(timeout_s=1.0)

            # --- Step 4: ASR ---
            transcript = mock_asr(rx)
            if not transcript:
                continue  # 静音

            print(f"[ai_agent] ASR: {transcript}")

            # --- Step 5: LLM → structured JSON ---
            reply_json = mock_llm(transcript)
            try:
                reply = json.loads(reply_json)
            except json.JSONDecodeError:
                print(f"[ai_agent] LLM 非 JSON: {reply_json}")
                continue

            action = reply.get("action", "silence")

            # --- Step 6: function call dispatch ---
            if action == "transfer":
                dest = reply.get("dest", transfer_dest)
                reason = reply.get("reason", "")
                print(f"[ai_agent] ★ LLM function call: transfer → dest={dest}  reason={reason}")
                ai_request_transfer(api_base, api_token, call_id, dest)
                break

            if action == "hangup":
                reason = reply.get("reason", "")
                print(f"[ai_agent] ★ LLM function call: hangup  reason={reason}")
                ai_hangup_via_uuid_kill(api_base, api_token, call_id)
                break

            if action == "say":
                text = reply.get("text", "")
                if not text:
                    continue

                # barge-in：说之前等用户说完
                if barge:
                    barge.wait_until_silence(timeout_s=1.0)

                print(f"[ai_agent] LLM say: {text}")
                reply_pcmu = mock_tts(text)
                if reply_pcmu:
                    ok = _send_tx(reply_pcmu)
                    if ok:
                        print(f"[ai_agent] ← TTS 回复 ({len(reply_pcmu)} bytes)")
                    else:
                        print(f"[ai_agent] ← TTS 发送失败！", file=sys.stderr)

            elif action == "silence":
                continue  # 用户没说话 / barge-in 中
            else:
                print(f"[ai_agent] 未知 action: {action}  json={reply_json}", file=sys.stderr)

    except KeyboardInterrupt:
        print("\n[ai_agent] Ctrl+C 退出")
    finally:
        # 清理：WS transport handler 自动 producer=0；HTTP 手动调一次保持幂等
        if ws_transport:
            ws_transport.close()
            print("[ai_agent] WS 已断开 → handler 自动 producer=0")
        ai_set_producer(api_base, api_token, call_id, producer=0)
        print("[ai_agent] producer 已切回 demo 模式；VAD 已停")


def main():
    p = argparse.ArgumentParser(description="REQ-AI-MEDIA-001 + REQ-AI-BARGEIN-001 + REQ-AI-WS-001: External AI Agent")
    p.add_argument("--api-base", default="https://nsbc-web.local:8080")
    p.add_argument("--api-token", required=True, help="Admin scope ext_key")
    p.add_argument("--call-id", required=True, help="从 SSE CALL_ANSWERED 事件拿到的 call_id")
    p.add_argument("--transfer-dest", default="1001", help="transfer 默认目标（默认 1001）")
    p.add_argument("--transport", choices=["http", "ws", "auto"], default="auto",
                   help="媒体流传输：http=HTTP polling+base64  ws=WebSocket Binary frame  auto=优先 WS 降级 HTTP（默认 auto）")
    p.add_argument("--vad-barge-in", dest="barge_in", action="store_true", default=True,
                   help="启用 barge-in（默认开）")
    p.add_argument("--no-vad-barge-in", dest="barge_in", action="store_false",
                   help="禁用 barge-in")
    p.add_argument("--barge-in-rms", type=int, default=800,
                   help="barge-in VAD RMS 阈值（默认 800，和 nsbcd 默认对齐）")
    p.add_argument("--chunk-bytes", type=int, default=160,
                   help="每次 get_rx 拉多少字节（默认 160 = 20ms PCMU）")
    args = p.parse_args()
    ai_loop(args.api_base, args.api_token, args.call_id, args.transfer_dest,
            args.barge_in, args.barge_in_rms, args.chunk_bytes,
            transport=args.transport)


if __name__ == "__main__":
    main()
