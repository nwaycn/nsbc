"""NsbcMediaClient — NSBC 外部 AI 媒体 API 的统一封装

Transport 层抽象：
  HttpTransport:  HTTP polling（ai-get-rx / ai-send-tx，base64 编解码）
                  零第三方依赖（urllib），兼容所有环境
  WsTransport:    WebSocket Binary frame（/ws/ai-media/:call_id）
                  零 base64 开销，延迟更低，需要 websockets 库

两个 Transport 暴露相同的 Rx/Tx 签名：
  get_rx(max_bytes) -> bytes   # 拉用户语音（PCMU 8kHz 原始字节）
  send_tx(pcmu) -> bool        # 推回复音频（PCMU 8kHz）

NsbcMediaClient = Transport + 控制命令（producer / vad / transfer / hangup）
"""
from __future__ import annotations

import base64
import json
import ssl
import time
import urllib.request
import urllib.error
from dataclasses import dataclass
from typing import Optional, Protocol


def _ssl_ctx() -> ssl.SSLContext:
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


def _cmd(api_base: str, api_token: str, command: str, timeout_s: int = 10) -> tuple[int, str]:
    """POST /ext/v1/cmd — 透明代理 nsbcd OAM 命令。返回 (http_rc, output)。http_rc=0=成功。"""
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
# Transport protocol + implementations
# =============================================================================

class MediaTransport(Protocol):
    """媒体流 transport 抽象。http / ws 都实现这 4 个方法。"""

    def connect(self, timeout_s: float = 10.0) -> bool: ...
    def disconnect(self) -> None: ...
    def get_rx(self, max_bytes: int = 160) -> bytes: ...
    def send_tx(self, pcmu_bytes: bytes) -> bool: ...


class HttpTransport:
    """HTTP polling transport — 零第三方依赖，所有环境通吃"""

    def __init__(self, api_base: str, api_token: str, call_id: str):
        self.api_base = api_base
        self.api_token = api_token
        self.call_id = call_id

    def connect(self, timeout_s: float = 10.0) -> bool:
        # HTTP transport 不需要 pre-connect — producer=1 由上层 NsbcMediaClient 调
        return True

    def disconnect(self) -> None:
        pass

    def get_rx(self, max_bytes: int = 160) -> bytes:
        rc, out = _cmd(self.api_base, self.api_token, f"ai-get-rx {self.call_id} {max_bytes}")
        if rc != 0:
            return b""
        for line in out.split("\n"):
            if line.startswith("b64:"):
                try:
                    return base64.b64decode(line[4:].strip())
                except Exception:
                    return b""
        return b""

    def send_tx(self, pcmu_bytes: bytes) -> bool:
        """推回复音频。> 3KB 自动分片（命令行 4KB 限制 → base64 膨胀 4/3 → cap 2.4KB 原始字节）。"""
        if not pcmu_bytes:
            return True
        chunk_cap = (4096 - 80) * 3 // 4  # ~2997 bytes 原始 PCMU
        for i in range(0, len(pcmu_bytes), chunk_cap):
            chunk = pcmu_bytes[i:i + chunk_cap]
            b64 = base64.b64encode(chunk).decode("ascii")
            rc, out = _cmd(self.api_base, self.api_token, f"ai-send-tx {self.call_id} 0 {b64}")
            if not _check_ok(rc, out):
                print(f"[HttpTransport] send_tx FAILED chunk={i} rc={rc} out={out.strip()[:100]}", flush=True)
                return False
        return True


class WsTransport:
    """WebSocket Binary frame transport — 零 base64 开销，需要 websockets 库

    协议：GET /ws/ai-media/:call_id?token=<admin-ext-key> → Binary frame 双向直传 PCMU
    producer=1 / VAD start 由 WS handler 自动处理（连接时触发）。"""

    def __init__(self, api_base: str, api_token: str, call_id: str):
        self.api_base = api_base
        self.api_token = api_token
        self.call_id = call_id
        self._ws = None
        self._loop = None
        self._rx_queue: list[bytes] = []
        self._rx_index = 0
        self._connected = False

    @property
    def available(self) -> bool:
        try:
            import websockets  # noqa: F401
            return True
        except ImportError:
            return False

    def _api_base_to_ws(self) -> str:
        base = self.api_base.rstrip("/")
        if base.startswith("https://"):
            scheme = "wss"
            rest = base[len("https://"):]
        elif base.startswith("http://"):
            scheme = "ws"
            rest = base[len("http://"):]
        else:
            scheme = "ws"
            rest = base
        return f"{scheme}://{rest}/ws/ai-media/{self.call_id}?token={self.api_token}"

    def connect(self, timeout_s: float = 10.0) -> bool:
        if not self.available:
            print("[WsTransport] websockets 库未安装，请 pip install websockets", flush=True)
            return False
        import asyncio, websockets as _ws

        try:
            self._loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self._loop)
            uri = self._api_base_to_ws()

            async def _async_connect():
                ssl_ctx = _ssl_ctx() if uri.startswith("wss://") else None
                self._ws = await asyncio.wait_for(_ws.connect(uri, ssl=ssl_ctx), timeout=timeout_s)

            self._loop.run_until_complete(_async_connect())
            self._connected = True
            print(f"[WsTransport] 已连接 → handler 自动 producer=1 + VAD on", flush=True)
            return True
        except Exception as e:
            print(f"[WsTransport] connect 失败: {e}", flush=True)
            self._connected = False
            return False

    def disconnect(self) -> None:
        if self._loop is None or self._ws is None:
            return
        import asyncio

        async def _async_close():
            try:
                await self._ws.close(code=1000, reason="agent shutdown")
            except Exception:
                pass

        try:
            self._loop.run_until_complete(_async_close())
        except Exception:
            pass
        self._connected = False
        try:
            self._loop.close()
        except Exception:
            pass
        self._loop = None

    def get_rx(self, max_bytes: int = 160) -> bytes:
        if not self._connected or self._loop is None:
            return b""
        import asyncio

        # 先消费已有 buffer
        if self._rx_queue:
            frame = self._rx_queue[0]
            remaining = len(frame) - self._rx_index
            if remaining <= max_bytes:
                self._rx_index = 0
                self._rx_queue.pop(0)
                return frame[:remaining]
            chunk = frame[self._rx_index:self._rx_index + max_bytes]
            self._rx_index += max_bytes
            return chunk

        # buffer 空 — 尝试 recv（50ms timeout）
        async def _async_get_rx():
            try:
                return await asyncio.wait_for(self._ws.recv(), timeout=0.05)
            except asyncio.TimeoutError:
                return None

        try:
            msg = self._loop.run_until_complete(_async_get_rx())
        except Exception:
            self._connected = False
            return b""

        if msg is None:
            return b""
        if isinstance(msg, bytes):
            if len(msg) > max_bytes:
                self._rx_queue.append(msg)
                self._rx_index = max_bytes
                return msg[:max_bytes]
            return msg
        return b""

    def send_tx(self, pcmu_bytes: bytes) -> bool:
        if not self._connected or not pcmu_bytes or self._loop is None:
            return True
        import asyncio

        async def _async_send():
            await self._ws.send(pcmu_bytes)

        try:
            self._loop.run_until_complete(_async_send())
            return True
        except Exception as e:
            print(f"[WsTransport] send_tx err: {e}", flush=True)
            self._connected = False
            return False


# =============================================================================
# NsbcMediaClient — 高层 API：Transport + 控制命令
# =============================================================================

@dataclass
class VADState:
    a_state: str   # "SPEECH" | "SILENCE"
    a_rms: int
    b_state: str
    b_rms: int
    a_active: bool
    b_active: bool


class NsbcMediaClient:
    """NSBC 外部 AI 媒体 API 客户端。

    用法：
      client = NsbcMediaClient(api_base, api_token, call_id, transport="auto")
      if not client.connect(): return

      # 主循环
      while True:
          rx = client.get_rx(chunk_bytes)
          if rx:
              transcript = asr.transcribe(rx)
              reply = llm.chat(transcript)
              if reply.action == "say":
                  pcmu = tts.synthesize(reply.text)
                  client.send_tx(pcmu)
              elif reply.action == "transfer":
                  client.request_transfer(reply.dest)
                  break
              ...

      client.disconnect()"""

    def __init__(self,
                 api_base: str,
                 api_token: str,
                 call_id: str,
                 transport: str = "auto"):
        self.api_base = api_base
        self.api_token = api_token
        self.call_id = call_id
        self._transport_name = transport
        self._transport: Optional[MediaTransport] = None

    # ---- transport lifecycle ----

    def connect(self, timeout_s: float = 10.0) -> bool:
        name = self._transport_name
        if name in ("ws", "auto"):
            ws = WsTransport(self.api_base, self.api_token, self.call_id)
            if ws.available and ws.connect(timeout_s):
                self._transport = ws
                return True
            if name == "ws":
                print("[NsbcMediaClient] 指定 ws 但连接失败", flush=True)
                return False
            print("[NsbcMediaClient] WS 不可用，降级 HTTP", flush=True)

        # HTTP fallback
        self._transport = HttpTransport(self.api_base, self.api_token, self.call_id)
        ok = self._transport.connect(timeout_s)
        if ok:
            # HTTP 模式下需要手动启 producer（WS handler 自动做）
            self.set_producer(1)
        return ok

    def disconnect(self) -> None:
        if self._transport:
            self._transport.disconnect()
        # 幂等清理：WS handler 的 producer=0 可能还没执行，再手动调一次
        if isinstance(self._transport, HttpTransport):
            self.set_producer(0)

    # ---- media: 委托 transport ----

    def get_rx(self, max_bytes: int = 160) -> bytes:
        return self._transport.get_rx(max_bytes) if self._transport else b""

    def send_tx(self, pcmu_bytes: bytes) -> bool:
        if self._transport and pcmu_bytes:
            return self._transport.send_tx(pcmu_bytes)
        return True

    # ---- control: HTTP 命令（这些不走媒体流，始终 HTTP 调用）----

    def set_producer(self, producer: int) -> bool:
        """producer=1 开启外部 AI 接管（自动启 VAD）；producer=0 切回 demo 模式（自动停 VAD）。"""
        rc, out = _cmd(self.api_base, self.api_token, f"ai-set-producer {self.call_id} {producer}")
        ok = _check_ok(rc, out)
        print(f"[nsbc] set_producer call={self.call_id} producer={producer} → {out.strip()[:150]}")
        return ok

    def vad_state(self) -> VADState:
        """查询 VAD 状态（barge-in 用）。"""
        rc, out = _cmd(self.api_base, self.api_token, f"ai-vad-state {self.call_id}")
        r = VADState("SILENCE", 0, "SILENCE", 0, False, False)
        if rc != 0:
            return r
        for line in out.split("\n"):
            line = line.strip()
            if line.startswith("OK A_leg="):
                parts = line.replace("OK A_leg=", "").split()
                r.a_state = parts[0] if parts else "SILENCE"
                if len(parts) > 1:
                    try: r.a_rms = int(parts[1].replace("rms=", ""))
                    except ValueError: pass
                r.a_active = "vad_active=1" in line
            elif line.startswith("OK B_leg="):
                parts = line.replace("OK B_leg=", "").split()
                r.b_state = parts[0] if parts else "SILENCE"
                if len(parts) > 1:
                    try: r.b_rms = int(parts[1].replace("rms=", ""))
                    except ValueError: pass
                r.b_active = "vad_active=1" in line
        return r

    def request_transfer(self, dest: str) -> bool:
        """请求转人工座席（blind REFER，非阻塞）。"""
        rc, out = _cmd(self.api_base, self.api_token, f"ai-request-transfer {self.call_id} {dest}")
        ok = _check_ok(rc, out)
        print(f"[nsbc] ★ request_transfer call={self.call_id} dest={dest} → {out.strip()[:120]}")
        return ok

    def hangup(self) -> bool:
        """挂断通话（uuid-kill）。"""
        rc, out = _cmd(self.api_base, self.api_token, f"uuid-kill {self.call_id}")
        ok = _check_ok(rc, out)
        print(f"[nsbc] ★ hangup call={self.call_id} → {out.strip()[:120]}")
        return ok
