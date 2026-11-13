# -*- coding: utf-8 -*-
"""
NSBC SIP-over-WSS 测试客户端底座。

封装 WebSocket 连接、Digest MD5 鉴权、REGISTER / INVITE / BYE / OPTIONS 全流程。
严格对齐 Android / Desktop / iOS 三端实现的 SIP 行为：
    - Via: SIP/2.0/WSS <local_ip>;branch=z9hG4bK-...;rport
    - Contact: <sip:<user>@<realm>;transport=ws>
    - Supported: replaces, outbound
    - Session-ID: <tag>@<server>
    - OPTIONS keepalive (30s, miss≥3 重注册)
    - 仅在 INVITE / OPTIONS 中带 Supported / Session-ID（REGISTER 不带）

用途：`test_mutual_calls.py` 模拟任意一个客户端，执行互呼场景。
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import hmac
import os
import random
import socket
import ssl
import time
import uuid
from dataclasses import dataclass
from typing import Optional


# ---------------------------------------------------------------------------
# SIP 基础工具
# ---------------------------------------------------------------------------

Z9HG4BK = "z9hG4bK-"


def new_branch() -> str:
    return Z9HG4BK + uuid.uuid4().hex


def new_tag() -> str:
    return uuid.uuid4().hex[:16]


def new_call_id(server: str = "") -> str:
    suffix = f"@{server}" if server else ""
    return f"{uuid.uuid4().hex}{suffix}"


def local_ipv4() -> str:
    """尽量返回本机对外的 IPv4，失败退回 127.0.0.1。"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def parse_sip_message(data: str) -> dict:
    """把原始 SIP 报文拆成 start_line + headers + body。"""
    parts = data.split("\r\n\r\n", 1)
    head = parts[0]
    body = parts[1] if len(parts) > 1 else ""
    lines = head.split("\r\n")
    start = lines[0]
    headers: dict[str, list[str]] = {}
    for line in lines[1:]:
        if not line:
            continue
        # 支持多值头（Via 多条）
        k, _, v = line.partition(":")
        k = k.strip()
        headers.setdefault(k, []).append(v.strip())
    return {"start": start, "headers": headers, "body": body}


def fmtp_param(line: str, param: str) -> Optional[str]:
    """从 a=fmtp:xx ... 里取出某个参数的值。"""
    # a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f
    idx = line.find(param + "=")
    if idx < 0:
        return None
    rest = line[idx + len(param) + 1:]
    end = rest.find(";")
    return rest[:end if end >= 0 else len(rest)]


# ---------------------------------------------------------------------------
# Digest MD5 鉴权
# ---------------------------------------------------------------------------

def _ha1(user: str, realm: str, password: str) -> str:
    return hashlib.md5(f"{user}:{realm}:{password}".encode()).hexdigest()


def _ha2(method: str, uri: str) -> str:
    return hashlib.md5(f"{method}:{uri}".encode()).hexdigest()


def compute_digest_response(
    *,
    user: str,
    password: str,
    realm: str,
    nonce: str,
    uri: str,
    method: str,
    qop: Optional[str] = None,
    nc: str = "00000001",
    cnonce: str = "",
) -> str:
    ha1 = _ha1(user, realm, password)
    ha2 = _ha2(method, uri)
    if qop and "auth" in qop.split(","):
        if not cnonce:
            cnonce = uuid.uuid4().hex[:16]
        resp = hashlib.md5(
            f"{ha1}:{nonce}:{nc}:{cnonce}:auth:{ha2}".encode()
        ).hexdigest()
        return (
            f'username="{user}", realm="{realm}", nonce="{nonce}", '
            f'uri="{uri}", response="{resp}", qop=auth, '
            f'nc={nc}, cnonce="{cnonce}"'
        )
    resp = hashlib.md5(f"{ha1}:{nonce}:{ha2}".encode()).hexdigest()
    return (
        f'username="{user}", realm="{realm}", nonce="{nonce}", '
        f'uri="{uri}", response="{resp}"'
    )


def parse_www_authenticate(header: str) -> dict:
    """
    把 WWW-Authenticate: Digest realm="...", nonce="...", qop="auth" 解析成 dict。
    """
    result: dict = {}
    # 去掉前缀 Digest
    line = header.strip()
    if line.lower().startswith("digest"):
        line = line[6:].strip()
    # 按逗号切，但要处理引号里的逗号（本项目场景里没有，简化处理）
    parts = []
    cur = ""
    in_quote = False
    for ch in line:
        if ch == '"':
            in_quote = not in_quote
            cur += ch
        elif ch == "," and not in_quote:
            parts.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur.strip())
    for p in parts:
        if "=" not in p:
            continue
        k, _, v = p.partition("=")
        k = k.strip().lower()
        v = v.strip().strip('"')
        result[k] = v
    return result


# ---------------------------------------------------------------------------
# SIP 客户端
# ---------------------------------------------------------------------------


@dataclass
class CallInfo:
    call_id: str
    local_tag: str
    remote_tag: Optional[str] = None
    local_branch: str = ""
    remote_branch: Optional[str] = None
    remote_target: Optional[str] = None  # 来自 200 OK 的 Contact
    remote_uri: str = ""
    remote_sdp: Optional[str] = None
    local_sdp: Optional[str] = None
    established: bool = False
    is_incoming: bool = False
    local_cseq: int = 2  # INVITE 用 1，BYE 等用 2+


class NsbcSipClient:
    """
    模拟三端任意一个的 SIP 客户端，通过 WSS 连接 NSBC。
    """

    def __init__(
        self,
        *,
        server: str,
        realm: str,
        username: str,
        password: str,
        wss_port: int = 5062,
        ws_path: str = "/ws",
        use_tls: bool = True,
        platform_tag: str = "generic",  # android / desktop / ios / generic
        allow_self_signed: bool = True,
    ):
        self.server = server
        self.realm = realm
        self.username = username
        self.password = password
        self.wss_port = wss_port
        self.ws_path = ws_path if ws_path.startswith("/") else f"/{ws_path}"
        self.use_tls = use_tls
        self.platform_tag = platform_tag
        self.allow_self_signed = allow_self_signed

        self.local_ip = local_ipv4()
        self.via_protocol = "WSS" if use_tls else "WS"
        self.sip_uri = f"sip:{username}@{realm}"
        self.session_id = f"{new_tag()}@{server}"

        self.ws_url = (
            f"wss://{server}:{wss_port}{self.ws_path}"
            if use_tls
            else f"ws://{server}:{wss_port}{self.ws_path}"
        )
        self.contact_transport = "ws"
        self.user_agent = f"NSBC-{platform_tag}/test-harness"

        # 运行时状态
        self._ws: Optional[any] = None
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._recv_task: Optional[asyncio.Task] = None
        self._send_queue: asyncio.Queue[str] | None = None

        # 注册状态
        self.reg_tag = new_tag()
        self.reg_branch = new_branch()
        self.reg_call_id = new_call_id(realm)
        self.reg_cseq = 0
        self.authed = False

        # 对话状态机
        self._pending: dict[str, asyncio.Future] = {}  # Call-ID → Future[SipMessage]
        self._incoming_future: Optional[asyncio.Future[CallInfo]] = None
        self._current_call: Optional[CallInfo] = None

        # keepalive
        self._keepalive_task: Optional[asyncio.Task] = None
        self._keepalive_call_id: str = ""
        self._keepalive_cseq: int = 0
        self._keepalive_misses: int = 0

        # 日志回调
        self.on_log = lambda level, msg: print(f"[{self.username}][{level}] {msg}")

    # ---------- 连接 ----------

    async def connect(self):
        """建立 WSS 连接，进入消息循环。"""
        import websockets

        self._send_queue = asyncio.Queue()
        self._loop = asyncio.get_event_loop()

        ssl_ctx: Optional[ssl.SSLContext] = None
        if self.use_tls:
            ssl_ctx = ssl.create_default_context()
            if self.allow_self_signed:
                ssl_ctx.check_hostname = False
                ssl_ctx.verify_mode = ssl.CERT_NONE

        self.on_log("INFO", f"连接 {self.ws_url}")
        self._ws = await websockets.connect(
            self.ws_url,
            additional_headers={"Sec-WebSocket-Protocol": "sip"},
            ssl=ssl_ctx,
            # 某些 websockets 版本用 subprotocols 参数
            subprotocols=["sip"],
            max_size=1024 * 1024,
        )
        self.on_log("INFO", "WebSocket 已连接 (SubProtocol=sip)")
        self._recv_task = asyncio.create_task(self._recv_loop())

    async def disconnect(self):
        """关闭连接。"""
        if self._keepalive_task:
            self._keepalive_task.cancel()
            self._keepalive_task = None
        if self._recv_task:
            self._recv_task.cancel()
            self._recv_task = None
        if self._ws:
            try:
                await self._ws.close()
            except Exception:
                pass
            self._ws = None
        self.on_log("INFO", "已断开")

    async def _recv_loop(self):
        try:
            assert self._ws is not None
            async for raw in self._ws:
                if isinstance(raw, bytes):
                    raw = raw.decode("utf-8", errors="replace")
                await self._handle_incoming(raw)
        except asyncio.CancelledError:
            raise
        except Exception as e:
            self.on_log("WARN", f"接收循环退出: {e}")

    async def _send(self, data: str):
        assert self._ws is not None
        await self._ws.send(data)

    # ---------- 发送构造 ----------

    def _build_register(self, *, with_auth: bool, expires: int) -> str:
        self.reg_cseq += 1
        msg = self._new_request("REGISTER", f"sip:{self.realm}")
        msg += f"Via: SIP/2.0/{self.via_protocol} {self.local_ip};branch={self.reg_branch};rport\r\n"
        msg += "Max-Forwards: 70\r\n"
        msg += f'From: "{self.username}" <{self.sip_uri}>;tag={self.reg_tag}\r\n'
        msg += f"To: <{self.sip_uri}>\r\n"
        msg += f"Call-ID: {self.reg_call_id}\r\n"
        msg += f"CSeq: {self.reg_cseq} REGISTER\r\n"
        msg += f"Contact: <{self.sip_uri};transport={self.contact_transport}>\r\n"
        msg += f"Expires: {expires}\r\n"
        msg += f"User-Agent: {self.user_agent}\r\n"
        if with_auth:
            msg += f"Authorization: {self._build_auth_header('REGISTER', f'sip:{self.realm}')}\r\n"
        msg += "\r\n"
        return msg

    def _build_invite(self, target_uri: str, offer_sdp: str, call: CallInfo) -> str:
        call.local_tag = call.local_tag or new_tag()
        call.local_branch = call.local_branch or new_branch()
        msg = self._new_request("INVITE", target_uri)
        msg += f"Via: SIP/2.0/{self.via_protocol} {self.local_ip};branch={call.local_branch};rport\r\n"
        msg += "Max-Forwards: 70\r\n"
        msg += f'From: "{self.username}" <{self.sip_uri}>;tag={call.local_tag}\r\n'
        msg += f"To: <{target_uri}>\r\n"
        msg += f"Call-ID: {call.call_id}\r\n"
        msg += f"CSeq: 1 INVITE\r\n"
        msg += f"Supported: replaces, outbound\r\n"
        msg += f"Session-ID: {self.session_id}\r\n"
        msg += f"Contact: <{self.sip_uri};transport={self.contact_transport}>\r\n"
        msg += f"User-Agent: {self.user_agent}\r\n"
        msg += "Content-Type: application/sdp\r\n"
        msg += f"Content-Length: {len(offer_sdp.encode())}\r\n\r\n"
        msg += offer_sdp
        return msg

    def _build_bye(self, call: CallInfo) -> str:
        if not call.remote_target:
            call.remote_target = f"sip:{self.realm}"
        msg = self._new_request("BYE", call.remote_target)
        msg += f"Via: SIP/2.0/{self.via_protocol} {self.local_ip};branch={new_branch()};rport\r\n"
        msg += "Max-Forwards: 70\r\n"
        from_tag = call.local_tag
        to_tag = call.remote_tag or ""
        msg += f'From: "{self.username}" <{self.sip_uri}>;tag={from_tag}\r\n'
        msg += f"To: <{call.remote_uri}>;tag={to_tag}\r\n"
        msg += f"Call-ID: {call.call_id}\r\n"
        msg += f"CSeq: {call.local_cseq} BYE\r\n"
        msg += f"Supported: replaces, outbound\r\n"
        msg += f"Session-ID: {self.session_id}\r\n"
        msg += f"User-Agent: {self.user_agent}\r\n"
        msg += "\r\n"
        return msg

    def _new_request(self, method: str, request_uri: str) -> str:
        return f"{method} {request_uri} SIP/2.0\r\n"

    def _build_auth_header(self, method: str, uri: str) -> str:
        if not getattr(self, "_last_challenge", None):
            return ""
        chal = self._last_challenge
        cnonce = uuid.uuid4().hex[:16]
        nc = f"{self.reg_cseq:08x}" if method == "REGISTER" else "00000001"
        return compute_digest_response(
            user=self.username,
            password=self.password,
            realm=chal["realm"],
            nonce=chal["nonce"],
            uri=uri,
            method=method,
            qop=chal.get("qop"),
            nc=nc,
            cnonce=cnonce,
        )

    # ---------- 入站处理 ----------

    async def _handle_incoming(self, raw: str):
        if not raw.strip():
            return
        msg = parse_sip_message(raw)
        start = msg["start"]
        headers = msg["headers"]

        self.on_log("RECV", f"{start[:80]}")

        # --- 响应 ---
        if "SIP/2.0" in start:
            status_code = int(start.split()[1])
            via_branch = headers.get("Via", [""])[0]
            branch = ""
            for tok in via_branch.split(";"):
                if tok.startswith("branch="):
                    branch = tok[7:]
            cseq_line = headers.get("CSeq", ["0"])[0]
            method = cseq_line.split()[-1] if cseq_line else ""
            call_id = headers.get("Call-ID", [""])[0].strip()

            if branch == self.reg_branch or call_id == self.reg_call_id:
                await self._handle_register_response(status_code, msg)
                return
            elif method == "OPTIONS" and call_id.startswith(self._keepalive_call_id):
                await self._handle_keepalive_response(status_code)
                return
            else:
                fut = self._pending.get(call_id)
                if fut and not fut.done():
                    fut.set_result({"status": status_code, "headers": headers, "body": msg["body"]})
                return

        # --- 请求 ---
        method = start.split()[0]
        call_id = headers.get("Call-ID", [""])[0].strip()

        if method == "INVITE":
            await self._handle_incoming_invite(msg)
        elif method == "OPTIONS":
            await self._answer_options(msg)
        elif method == "BYE":
            await self._handle_bye(msg)

    async def _handle_register_response(self, code: int, msg: dict):
        if code == 401 or code == 407:
            www = msg["headers"].get("WWW-Authenticate") or msg["headers"].get("Proxy-Authenticate", [])
            if www:
                self._last_challenge = parse_www_authenticate(www[0])
            self.on_log("INFO", f"REGISTER 收到 {code} 挑战，带 Authorization 重试")
            # 重置 branch 避免与上一轮混淆，也顺便支持多次 auth 场景
            self.reg_branch = new_branch()
            self.reg_cseq = 0  # _build_register 会自增
            body = self._build_register(with_auth=True, expires=300)
            await self._send(body)
        elif 200 <= code < 300:
            self.authed = True
            self.on_log("INFO", f"✅ REGISTER 成功 ({code})")
            await self._start_keepalive()
        else:
            self.on_log("ERROR", f"❌ REGISTER 失败 {code}")

    async def _answer_options(self, msg: dict):
        """对端发来的 OPTIONS，回 200 OK + Allow / Supported。"""
        to_header = msg["headers"].get("To", [""])[0]
        to_tag = ""
        for tok in to_header.split(";"):
            if tok.startswith("tag="):
                to_tag = tok[4:]
        cseq = msg["headers"].get("CSeq", [""])[0]
        call_id = msg["headers"].get("Call-ID", [""])[0]
        branch = ""
        for tok in msg["headers"].get("Via", [""])[0].split(";"):
            if tok.startswith("branch="):
                branch = tok[7:]
        resp = (
            "SIP/2.0 200 OK\r\n"
            f"Via: {msg['headers'].get('Via', [''])[0]}\r\n"
            "Max-Forwards: 70\r\n"
            f"From: {msg['headers'].get('From', [''])[0]}\r\n"
            f"To: {to_header};tag={new_tag()}\r\n"
            f"Call-ID: {call_id}\r\n"
            f"CSeq: {cseq}\r\n"
            f"Supported: replaces, outbound\r\n"
            f"Session-ID: {self.session_id}\r\n"
            f"User-Agent: {self.user_agent}\r\n"
            "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, UPDATE, PRACK\r\n"
            "Content-Length: 0\r\n\r\n"
        )
        await self._send(resp)

    async def _handle_incoming_invite(self, msg: dict):
        """收到对端 INVITE — 自动 200 OK 应答并让上层拿到 CallInfo。"""
        headers = msg["headers"]
        call_id = headers.get("Call-ID", [""])[0].strip()
        from_header = headers.get("From", [""])[0]
        to_header = headers.get("To", [""])[0]
        via = headers.get("Via", [""])[0]
        cseq = headers.get("CSeq", [""])[0]
        remote_sdp = msg["body"]
        target_uri = to_header.split()[0].lstrip("<") if "<" in to_header else ""

        call = CallInfo(
            call_id=call_id,
            local_tag=new_tag(),
            remote_tag="",
            local_branch=new_branch(),
            remote_uri=target_uri,
            remote_sdp=remote_sdp,
            is_incoming=True,
        )
        # 提取远端 tag
        for tok in from_header.split(";"):
            if tok.startswith("tag="):
                call.remote_tag = tok[4:]

        self._current_call = call

        # 先回 180 Ringing
        branch = ""
        for tok in via.split(";"):
            if tok.startswith("branch="):
                branch = tok[7:]
        ringing = (
            "SIP/2.0 180 Ringing\r\n"
            f"Via: {via}\r\n"
            f"From: {from_header}\r\n"
            f"To: {to_header};tag={call.local_tag}\r\n"
            f"Call-ID: {call_id}\r\n"
            f"CSeq: {cseq}\r\n"
            f"User-Agent: {self.user_agent}\r\n"
            "Content-Length: 0\r\n\r\n"
        )
        await self._send(ringing)

        # 等一小下再 200 OK（模拟振铃）
        await asyncio.sleep(0.3)

        # 生成最小 SDP 应答
        local_sdp = self._generate_minimal_sdp(remote_sdp)
        call.local_sdp = local_sdp

        contact = f"<{self.sip_uri};transport={self.contact_transport}>"
        ok = (
            "SIP/2.0 200 OK\r\n"
            f"Via: {via}\r\n"
            f"From: {from_header}\r\n"
            f"To: {to_header};tag={call.local_tag}\r\n"
            f"Call-ID: {call_id}\r\n"
            f"CSeq: {cseq}\r\n"
            f"Supported: replaces, outbound\r\n"
            f"Session-ID: {self.session_id}\r\n"
            f"Contact: {contact}\r\n"
            f"User-Agent: {self.user_agent}\r\n"
            "Content-Type: application/sdp\r\n"
            f"Content-Length: {len(local_sdp.encode())}\r\n\r\n"
            f"{local_sdp}"
        )
        await self._send(ok)
        call.established = True

        # 唤醒等待的上层
        if self._incoming_future and not self._incoming_future.done():
            self._incoming_future.set_result(call)

    async def _handle_bye(self, msg: dict):
        headers = msg["headers"]
        via = headers.get("Via", [""])[0]
        from_header = headers.get("From", [""])[0]
        to_header = headers.get("To", [""])[0]
        call_id = headers.get("Call-ID", [""])[0]
        cseq = headers.get("CSeq", [""])[0]
        ok = (
            "SIP/2.0 200 OK\r\n"
            f"Via: {via}\r\n"
            f"From: {from_header}\r\n"
            f"To: {to_header}\r\n"
            f"Call-ID: {call_id}\r\n"
            f"CSeq: {cseq}\r\n"
            f"User-Agent: {self.user_agent}\r\n"
            "Content-Length: 0\r\n\r\n"
        )
        await self._send(ok)
        if self._current_call and self._current_call.call_id == call_id:
            self._current_call = None

    # ---------- keepalive ----------

    async def _start_keepalive(self):
        if self._keepalive_task:
            return
        self._keepalive_task = asyncio.create_task(self._keepalive_loop())

    async def _stop_keepalive(self):
        if self._keepalive_task:
            self._keepalive_task.cancel()
            self._keepalive_task = None

    async def _keepalive_loop(self):
        try:
            while True:
                await asyncio.sleep(30)
                await self._send_keepalive_options()
        except asyncio.CancelledError:
            pass

    async def _send_keepalive_options(self):
        if not self._keepalive_call_id:
            self._keepalive_call_id = new_call_id(self.realm)
        self._keepalive_cseq += 1
        msg = (
            "OPTIONS sip:" + self.realm + " SIP/2.0\r\n"
            f"Via: SIP/2.0/{self.via_protocol} {self.local_ip};branch={new_branch()};rport\r\n"
            "Max-Forwards: 70\r\n"
            f'From: "{self.username}" <{self.sip_uri}>;tag={self.reg_tag}\r\n'
            f"To: <sip:{self.realm}>\r\n"
            f"Call-ID: {self._keepalive_call_id}\r\n"
            f"CSeq: {self._keepalive_cseq} OPTIONS\r\n"
            f"Supported: replaces, outbound\r\n"
            f"Session-ID: {self.session_id}\r\n"
            f"User-Agent: {self.user_agent}\r\n"
            "\r\n"
        )
        await self._send(msg)

    async def _handle_keepalive_response(self, code: int):
        if 200 <= code < 300:
            self._keepalive_misses = 0
        else:
            self._keepalive_misses += 1
            self.on_log("WARN", f"Keepalive {code} (miss={self._keepalive_misses})")
            if self._keepalive_misses >= 3:
                self.on_log("WARN", "Keepalive 连续 3 次，触发重注册")
                await self.register()
                self._keepalive_misses = 0

    # ---------- 对外 API ----------

    async def register(self, expires: int = 300) -> bool:
        self.on_log("INFO", f"REGISTER expires={expires}")
        self.reg_branch = new_branch()
        self.reg_cseq = 0
        body = self._build_register(with_auth=False, expires=expires)
        await self._send(body)

        # 等待注册成功（超时 15s）
        deadline = time.time() + 15
        while time.time() < deadline:
            await asyncio.sleep(0.2)
            if self.authed:
                return True
        self.on_log("ERROR", "REGISTER 超时")
        return False

    async def unregister(self) -> bool:
        """注销 — REGISTER Expires=0。"""
        self.on_log("INFO", "UNREGISTER")
        self.reg_branch = new_branch()
        self.reg_cseq = 0
        body = self._build_register(with_auth=True, expires=0)
        await self._send(body)
        self.authed = False
        await self._stop_keepalive()
        await asyncio.sleep(0.5)
        return True

    async def call(self, target: str, offer_sdp: Optional[str] = None, timeout: float = 15.0) -> CallInfo:
        """
        主叫：向 target（形如 sip:10002@nsbc.local）发起呼叫，等待对端 200 OK。
        """
        call = CallInfo(
            call_id=new_call_id(self.realm),
            local_tag=new_tag(),
            remote_uri=target,
        )
        call.local_sdp = offer_sdp or self._generate_minimal_sdp()
        self._current_call = call

        pending = asyncio.Future()
        self._pending[call.call_id] = pending

        body = self._build_invite(target, call.local_sdp, call)
        self.on_log("INFO", f"INVITE → {target}")
        await self._send(body)

        try:
            result = await asyncio.wait_for(pending, timeout=timeout)
            code = result["status"]
            if 200 <= code < 300:
                # 处理 200 OK — 拿到对端 Contact 和 SDP，回 ACK
                headers = result["headers"]
                call.remote_target = headers.get("Contact", [""])[0].split(";")[0].strip("<>")
                call.remote_sdp = result["body"] or None
                # 提取远端 tag
                for tok in headers.get("To", [""])[0].split(";"):
                    if tok.startswith("tag="):
                        call.remote_tag = tok[4:]
                # 发 ACK
                ack = self._build_ack(call)
                await self._send(ack)
                call.established = True
                self.on_log("INFO", f"✅ 呼叫接通 ({call.call_id[:8]})")
                return call
            else:
                self.on_log("ERROR", f"INVITE 收到 {code}，呼叫失败")
                raise RuntimeError(f"INVITE failed: {code}")
        finally:
            self._pending.pop(call.call_id, None)

    def _build_ack(self, call: CallInfo) -> str:
        target = call.remote_target or call.remote_uri
        msg = self._new_request("ACK", target)
        msg += f"Via: SIP/2.0/{self.via_protocol} {self.local_ip};branch={new_branch()};rport\r\n"
        msg += "Max-Forwards: 70\r\n"
        msg += f'From: "{self.username}" <{self.sip_uri}>;tag={call.local_tag}\r\n'
        msg += f"To: <{call.remote_uri}>;tag={call.remote_tag or ''}\r\n"
        msg += f"Call-ID: {call.call_id}\r\n"
        msg += f"CSeq: 1 ACK\r\n"
        msg += f"User-Agent: {self.user_agent}\r\n"
        msg += "\r\n"
        return msg

    async def wait_for_incoming(self, timeout: float = 30.0) -> CallInfo:
        """等待一通呼入（用于被叫端）。"""
        self._incoming_future = asyncio.Future()
        try:
            call = await asyncio.wait_for(self._incoming_future, timeout=timeout)
            self.on_log("INFO", f"✅ 呼入接通 ({call.call_id[:8]})")
            return call
        finally:
            self._incoming_future = None

    async def hangup(self) -> None:
        if not self._current_call:
            return
        call = self._current_call
        bye = self._build_bye(call)
        self.on_log("INFO", f"BYE → {call.call_id[:8]}")
        await self._send(bye)
        await asyncio.sleep(0.5)

    # ---------- SDP ----------

    def _generate_minimal_sdp(self, remote_sdp: Optional[str] = None) -> str:
        """
        生成一份可用于 offer / answer 的最小 SDP。
        H.264 profile 固定 baseline (42e01f + packetization-mode=1)，
        Opus 带 maxplaybackrate=8000，对齐三端过滤策略。
        """
        # 地址：对 answer 取远端 offer 的媒体地址，offer 用本机 IP
        o_addr = "127.0.0.1"
        if remote_sdp:
            for line in remote_sdp.split("\r\n"):
                if line.startswith("o="):
                    o_addr = line.split()[-1]
                    break

        audio_port = 40000
        video_port = 40002

        has_video = remote_sdp is None or any(
            line.startswith("m=video") for line in remote_sdp.split("\r\n")
        )

        sdp = (
            f"v=0\r\n"
            f"o=- {int(time.time())} 0 IN IP4 {o_addr}\r\n"
            f"s=- NSBC test\r\n"
            f"t=0 0\r\n"
            f"a=group:BUNDLE 0{' 1' if has_video else ''}\r\n"
            f"a=msid-semantic: WMS nsbc\r\n"
            f"m=audio {audio_port} RTP/SAVP 111 0 8 9\r\n"
            f"c=IN IP4 {o_addr}\r\n"
            f"a=candidate:0 1 udp 2130706431 {o_addr} {audio_port} typ host\r\n"
            f"a=ice-ufrag:test\r\n"
            f"a=ice-pwd:testpwd1234567890\r\n"
            f"a=fingerprint:sha-256 DummyFingerprint00000000000000000000000000000000000000000000000000000000\r\n"
            f"a=setup:actpass\r\n"
            f"a=mid:0\r\n"
            f"a=rtpmap:111 opus/48000/2\r\n"
            f"a=fmtp:111 maxplaybackrate=8000\r\n"
            f"a=rtpmap:0 PCMU/8000\r\n"
            f"a=rtpmap:8 PCMA/8000\r\n"
            f"a=rtpmap:9 G722/8000\r\n"
        )
        if has_video:
            sdp += (
                f"\r\nm=video {video_port} RTP/SAVP 96 97\r\n"
                f"c=IN IP4 {o_addr}\r\n"
                f"a=candidate:0 1 udp 2130706432 {o_addr} {video_port} typ host\r\n"
                f"a=ice-ufrag:test\r\n"
                f"a=ice-pwd:testpwd1234567890\r\n"
                f"a=fingerprint:sha-256 DummyFingerprint00000000000000000000000000000000000000000000000000000000\r\n"
                f"a=setup:actpass\r\n"
                f"a=mid:1\r\n"
                f"a=rtpmap:96 H264/90000\r\n"
                f"a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
                f"a=rtpmap:97 VP8/90000\r\n"
            )
        sdp += "\r\n"
        return sdp
