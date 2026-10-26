#!/usr/bin/env python3
"""NSBC Event/CDR Subscription & Originate Client (Python, stdlib only)

Connects to nsbcd via Unix Domain Socket (or nsbc-web HTTP API), subscribes to
real-time events/CDRs, and optionally originates test calls.

Designed for Debian 12 out-of-the-box (stdlib only, no pip install needed).

Modes:
  1. originate + subscribe (default):
     POST /originate to nsbc-web, then UDS-subscribe events/CDRs, correlate by
     call_id, print a timeline, and optionally webhook each frame.
  2. subscribe-only:
     Just connect UDS and sub events,cdr,log — pure observer.
  3. originate-only:
     Fire-and-forget POST /originate, print the server response and exit.

Default test call: originate user/<ext> &hangup (echo is reserved but not yet
implemented in nsbcd core; use &park to keep the call online for observation).

Frame format (nsbcd obs_sub unified single-line):
    events: YYYY-MM-DDTHH:MM:SS.mmm CALL_INCOMING  k=v k=v ...
    events: YYYY-MM-DDTHH:MM:SS.mmm CALL_ANSWERED  k=v k=v ...
    events: YYYY-MM-DDTHH:MM:SS.mmm CALL_END       k=v k=v ...
    cdr:    YYYY-MM-DDTHH:MM:SS.mmm CDR { ... JSON ... }

Usage:
    # Default: originate extension 1001 with hangup, observe its lifecycle
    python3 event_client.py

    # Originate a specific extension with park (call stays on hold)
    python3 event_client.py --orig-to 2001 --app park

    # Originate with VIDEO (一键视频: H264+VP8)
    python3 event_client.py --orig-to 7gja30rf --app echo --video

    # Originate with VIDEO + multiple audio codecs (让 WebRTC 自动协商最优)
    python3 event_client.py --orig-to 7gja30rf --app echo --video --audio

    # 精确控制 codec 列表
    python3 event_client.py --orig-to 2001 --app echo \\
        --audio-codecs PCMU,PCMA,G722,OPUS --video-codecs H264,VP8,VP9

    # Just watch all events/CDRs (production mode)
    python3 event_client.py --mode subscribe

    # Originate only, no subscription
    python3 event_client.py --mode originate --orig-to 1001 --video

    # Webhook every event to your call-center integration
    python3 event_client.py --video --webhook-url http://cc.example.com/nsbc/events

    # Custom UDS path and nsbc-web base
    python3 event_client.py --uds /var/run/nsbc/control.sock \\
                            --api-base http://127.0.0.1:8080

    # Interactive REPL over UDS (like nsbc_cli but Python)
    python3 event_client.py --repl
"""

import argparse
import json
import os
import re
import socket
import ssl
import sys
import time
import threading
import selectors
import urllib.request
import urllib.error
from datetime import datetime
from collections import OrderedDict

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

DEFAULT_UDS = "/var/run/nsbc/control.sock"
# 线上 nsbc-web 监听 0.0.0.0:443 TLS + domain=nsbc.opensips.net.cn
# 本机执行时 https://127.0.0.1:443 也可用（同机 curl 直接打 127.0.0.1:443）
# 允许通过 NSBC_API_BASE env 覆盖
DEFAULT_API_BASE = os.environ.get("NSBC_API_BASE", "https://127.0.0.1:443")
DEFAULT_TOPICS = "events,cdr"
DEFAULT_TIMEOUT = 30  # seconds to wait for CDR after originate

# 全局 SSL 验证开关。nsbc-web 用自签证书时 --insecure 关掉验证
_SSL_VERIFY = True


def _ssl_ctx():
    """Return SSL context for urllib. If _SSL_VERIFY=False, skip cert verification."""
    if not _SSL_VERIFY:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        return ctx
    return None  # None = urllib 用系统默认（验证证书）

PROMPT = "nsbc> "
BANNER_PREFIX = "NSBC "

FRAME_RE = re.compile(
    r'^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3})\s+(\S+)\s+(.*)$'
)
KV_RE = re.compile(r'(\w+)=(\S+)')

# ANSI colors (disabled automatically if stdout is not a tty)
_USE_COLOR = sys.stdout.isatty()

COLOR = {
    "CALL_INCOMING": "\033[36m",   # cyan
    "CALL_ANSWERED": "\033[32m",   # green
    "CALL_END":      "\033[31m",   # red
    "REGISTER":      "\033[33m",   # yellow
    "CDR":           "\033[35m",   # magenta
    "OK":            "\033[32m",
    "ERR":           "\033[31m",
}
RESET = "\033[0m"
DIM = "\033[2m"
BOLD = "\033[1m"


def _c(code):
    return code if _USE_COLOR else ""


# ---------------------------------------------------------------------------
# Originate: dual-path architecture
# ---------------------------------------------------------------------------
#
# nsbcd 支持两种 originate 触发方式：
#   A. UDS 控制 socket 发 "originate <call-url>"          ← 推荐，同机部署零开销
#      （nsbccd control.sock 0600 权限 = 系统级鉴权，无需额外 token）
#   B. nsbc-web POST /api/v1/originate （需要 JWT 或静态 API Key）
#      ← 远程部署或呼叫中心无法访问 UDS 时用
#
# Python 客户端默认 originate_via="auto"：
#   1) 先用 UDS（socket 存在且 os.access(W_OK) 通过 → 直接走 A）
#   2) UDS 不可用 → 回退 HTTP（需要 --api-token 或走不带 token 的公开 API）
#   3) 都不可用 → 返回失败


def originate_via_uds(uds_path, call_url, timeout_s=10):
    """Send 'originate <call_url>' over nsbcd Unix Domain Socket.

    同 nsbc_cli 的 originate 命令 —— 零鉴权烦恼，UDS 文件权限 0600 就是 auth。
    Returns (ok: bool, payload: str) where payload is the raw command response.
    """
    try:
        if not os.path.exists(uds_path):
            return False, f"UDS socket not found: {uds_path}"
        if not os.access(uds_path, os.W_OK):
            return False, f"no write permission on UDS: {uds_path} (need same user or root)"

        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout_s)
        s.connect(uds_path)

        cmd = f"originate {call_url}\n"
        s.sendall(cmd.encode("utf-8"))

        # Read until we see "OK" or "ERR" prefix and the trailing prompt
        buf = b""
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            buf += chunk
            text = buf.decode("utf-8", "replace")
            if "OK " in text or "ERR " in text:
                if text.rstrip().endswith(">") or ">" in text[-5:]:
                    break
            if len(buf) > 1_000_000:  # 安全阀
                break

        s.close()
        text = buf.decode("utf-8", "replace").strip()
        # Strip trailing prompt
        if text.endswith(">"):
            text = text[:-1].strip()
        if text.startswith("OK ") or "\nOK " in text or text == "OK":
            return True, text
        if text.startswith("ERR") or "ERR " in text:
            return False, text
        # 没看到 OK/ERR 但也没超时 —— 认为是成功（originate 是异步命令）
        return True, text if text else "dispatched (async)"

    except (socket.timeout, OSError) as e:
        return False, f"UDS originate failed: {e}"


def originate_via_web(api_base, call_url, timeout_s=10, api_token=None):
    """POST to nsbc-web /ext/v1/originate endpoint (REQ-EXT-API-001).

    /ext/v1 路由组走 API Key 鉴权（scope readwrite+），和 Web UI 的
    /api/v1/*（JWT + rbac）完全隔离。外部呼叫中心/CRM 只需要一个
    静态 API Key 就能调这个接口。

    Returns (ok: bool, payload: dict|str).
    """
    url = api_base.rstrip("/") + "/ext/v1/originate"
    body = json.dumps({"call_url": call_url}).encode("utf-8")
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json",
    }
    if api_token:
        headers["Authorization"] = f"Bearer {api_token}"
    req = urllib.request.Request(url, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout_s, context=_ssl_ctx()) as resp:
            data = resp.read().decode("utf-8", "replace")
            try:
                return True, json.loads(data)
            except json.JSONDecodeError:
                return True, data
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace") if hasattr(e, "read") else str(e)
        return False, {"http_error": e.code, "detail": detail}
    except (urllib.error.URLError, OSError) as e:
        return False, {"network_error": str(e)}


def originate(call_url, via, uds_path, api_base, api_token=None, timeout_s=10):
    """Unified originate entry — 按 via 选择 UDS / HTTP / auto。

    via: "auto" (default) | "uds" | "web"
    Returns (ok, payload, used_via).
    """
    if via == "uds":
        ok, payload = originate_via_uds(uds_path, call_url, timeout_s)
        return ok, payload, "uds"
    if via == "web":
        ok, payload = originate_via_web(api_base, call_url, timeout_s, api_token)
        return ok, payload, "web"

    # auto: 先试 UDS，UDS 不可用再 HTTP
    ok, payload = originate_via_uds(uds_path, call_url, timeout_s)
    if ok:
        return True, payload, "uds"
    # UDS 失败 → 回退 HTTP
    print_banner(f"[UDS originate failed ({payload}), falling back to HTTP...]")
    ok2, payload2 = originate_via_web(api_base, call_url, timeout_s, api_token)
    if ok2:
        return True, payload2, "web"
    return False, {"uds": payload, "web": payload2}, "both_failed"


# ---------------------------------------------------------------------------
# UDS control socket client (订阅事件)
# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# SSE client (alternative to UDS — remote deployment)
# ---------------------------------------------------------------------------

class SSEClient:
    """Minimal Server-Sent Events client consuming nsbc-web /calls/stream.

    nsbc-web SSE endpoints emit standard format:
        event: session
        data: {"kind":"...","payload":{...}}

    Each block is terminated by a blank line ('\\n\\n').
    """

    def __init__(self, url, timeout_s=None, headers=None):
        self.url = url
        self.timeout_s = timeout_s
        self.headers = headers or {}
        self._resp = None
        self._buf = ""

    def connect(self):
        req = urllib.request.Request(
            self.url,
            headers={
                "Accept": "text/event-stream",
                "Cache-Control": "no-cache",
                **self.headers,
            },
        )
        try:
            self._resp = urllib.request.urlopen(req, timeout=self.timeout_s, context=_ssl_ctx())
        except (urllib.error.URLError, OSError) as e:
            raise ConnectionError(f"SSE connect failed: {e}")
        content_type = self._resp.headers.get("Content-Type", "")
        if "text/event-stream" not in content_type and "application/octet-stream" not in content_type:
            # Still proceed — some proxies strip Content-Type
            pass
        return self

    def close(self):
        if self._resp:
            try:
                self._resp.close()
            except Exception:
                pass
            self._resp = None

    def iter_events(self):
        """Yield parsed SSE event dicts forever.

        Each yield: {"event": str, "data": str, "retry": int|None}
        Retries on EAGAIN/timeout by reconnecting.
        """
        while True:
            try:
                for ev in self._read_stream():
                    yield ev
                # Stream ended → reconnect loop continues
                print(f"{_c(DIM)}[sse: stream closed, reconnecting in 2s...]{_c(RESET)}", file=sys.stderr)
                time.sleep(2)
                self.connect()
            except (ConnectionError, OSError) as e:
                print(f"{_c(COLOR['ERR'])}[sse: {e}, reconnecting in 3s]{_c(RESET)}", file=sys.stderr)
                time.sleep(3)
                try:
                    self.connect()
                except Exception as e2:
                    print(f"{_c(COLOR['ERR'])}[sse reconnect still down: {e2}]{_c(RESET)}", file=sys.stderr)

    def _read_stream(self):
        """Internal: parse raw HTTP stream into SSE event dicts."""
        reader = self._resp
        event_name = "message"
        data_lines = []
        retry_ms = None

        while True:
            try:
                chunk = reader.readline()
                if not chunk:
                    break  # stream closed
            except (socket.timeout, TimeoutError):
                break
            except OSError:
                break

            line = chunk.decode("utf-8", "replace").rstrip("\n\r")

            if line == "":
                # End of event block
                if data_lines or event_name != "message":
                    data = "\n".join(data_lines)
                    yield {
                        "event": event_name,
                        "data": data,
                        "retry": retry_ms,
                    }
                # Reset for next event
                event_name = "message"
                data_lines = []
                retry_ms = None
                continue

            if line.startswith(":"):
                # SSE comment (heartbeat) — skip
                continue

            if line.startswith("event:"):
                event_name = line[6:].strip() or "message"
            elif line.startswith("data:"):
                val = line[5:]
                if val.startswith(" "):
                    val = val[1:]
                data_lines.append(val)
            elif line.startswith("retry:"):
                try:
                    retry_ms = int(line[6:].strip())
                except ValueError:
                    pass
            # Unknown field — ignore per SSE spec


def sse_frame_to_parsed(sse_event, source="sse"):
    """Convert an SSE event dict into the unified parsed frame format.

    nsbc-web /calls/stream emits SSE event=session, data is:
        {"kind":"session","payload":{"event":"ring","call_id":"...","state":"early",
                                     "from":"...","to":"...","dir":"...",...}}

    We map SSE 'event' values to the same uppercase 'domain' that UDS obs_sub
    frames produce, so CallCorrelator and print_frame_parsed work identically
    across both transports.
    """
    ev = sse_event
    data_str = ev.get("data", "")

    try:
        blob = json.loads(data_str)
    except json.JSONDecodeError:
        return None

    kind_outer = blob.get("kind", "unknown")
    payload = blob.get("payload", {})

    if not isinstance(payload, dict):
        return None

    # Map SSE inner 'event' → unified domain (same as UDS obs_sub)
    sse_evt = payload.get("event", "").lower()
    SSE_EVENT_TO_DOMAIN = {
        "ring": "CALL_INCOMING",     # SSE ring = UDS CALL_INCOMING (inbound early)
        "answer": "CALL_ANSWERED",   # SSE answer = UDS CALL_ANSWERED
        "hangup": "CALL_END",        # SSE hangup = UDS CALL_END
        "originate": "CALL_OUTGOING",# SSE originate = outbound call start
    }
    domain = SSE_EVENT_TO_DOMAIN.get(sse_evt, sse_evt.upper() or kind_outer.upper())

    # If SSE didn't have an inner 'event' field (e.g. lag already filtered above),
    # still emit something so the user sees it
    if not sse_evt and domain == "SESSION":
        # Generic session event — keep as-is but note source
        pass

    ts = payload.get("ts", payload.get("timestamp", ""))
    if not ts:
        now = datetime.now()
        ts = now.strftime("%Y-%m-%dT%H:%M:%S.") + f"{now.microsecond // 1000:03d}"

    return {
        "ts": ts,
        "ts_str": ts,
        "domain": domain,
        "kind": payload.get("event", kind_outer),
        "fields": payload,
        "raw": json.dumps(blob, default=str),
        "_source": source,
    }


# ---------------------------------------------------------------------------
# UDS control socket client
# ---------------------------------------------------------------------------

class UDSClient:
    """Thin wrapper around nsbcd Unix Domain Socket control interface."""

    def __init__(self, uds_path, connect_timeout=30):
        self.uds_path = uds_path
        self.sock = None
        self._banner = ""
        self._lock = threading.Lock()

    def connect(self):
        if not os.path.exists(self.uds_path):
            raise FileNotFoundError(
                f"UDS socket not found: {self.uds_path}\n"
                f"  Did nsbcd start? Check config 'uds_control.path'."
            )
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(30)
        s.connect(self.uds_path)
        s.settimeout(None)
        self.sock = s
        # Read initial banner/prompt (non-blocking probe)
        try:
            s.settimeout(1.0)
            chunk = s.recv(4096)
            if chunk:
                self._banner = chunk.decode("utf-8", "replace")
            s.settimeout(None)
        except socket.timeout:
            s.settimeout(None)
        return self

    @property
    def banner(self):
        return self._banner

    def close(self):
        if self.sock:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def send_command(self, cmd, wait_prompt=True, timeout_s=2.0):
        """Send a single command and read up to the next nsbc> prompt.

        Returns the raw response string (without the trailing prompt).
        """
        with self._lock:
            self.sock.sendall((cmd.rstrip("\n") + "\n").encode("utf-8"))
            if not wait_prompt:
                return ""
            return self._recv_until_prompt(timeout_s)

    def _recv_until_prompt(self, timeout_s):
        self.sock.setblocking(False)
        buf = b""
        sel = selectors.DefaultSelector()
        sel.register(self.sock, selectors.EVENT_READ)
        deadline = time.time() + timeout_s
        try:
            while time.time() < deadline:
                for _ in sel.select(timeout=0.2):
                    try:
                        chunk = self.sock.recv(65536)
                    except BlockingIOError:
                        continue
                    if not chunk:
                        break
                    buf += chunk
                    if PROMPT.encode() in buf:
                        break
                if PROMPT.encode() in buf:
                    break
        finally:
            self.sock.setblocking(True)
            sel.close()
        text = buf.decode("utf-8", "replace")
        # Strip trailing prompt
        if text.endswith(PROMPT):
            text = text[: -len(PROMPT)]
        elif PROMPT in text:
            text = text[: text.rindex(PROMPT)]
        return text

    def subscribe(self, topics):
        """Send sub command, return server response."""
        return self.send_command(f"sub {topics}")

    def iter_frames(self):
        """Yield lines pushed by nsbcd (events/CDRs/logs/metrics).

        Each yield is one complete frame (stripped of trailing newline).
        Blocks until connection closes.
        """
        for raw in self.sock.makefile("r", buffering=1):
            line = raw.rstrip("\n\r")
            if line:
                yield line


# ---------------------------------------------------------------------------
# Frame parsing
# ---------------------------------------------------------------------------

def parse_frame(line):
    """Parse a single obs_sub frame line into a structured dict.

    Returns a dict with keys: ts, domain, kind, fields (dict), raw.
    Returns None if the line is not a recognized frame format.
    """
    line = line.rstrip("\n\r")
    if not line:
        return None

    m = FRAME_RE.match(line)
    if not m:
        return None

    ts_s, domain, body = m.group(1), m.group(2), m.group(3)

    try:
        ts = datetime.strptime(ts_s, "%Y-%m-%dT%H:%M:%S.%f")
    except ValueError:
        ts = ts_s

    result = {
        "ts": ts,
        "ts_str": ts_s,
        "domain": domain,
        "kind": _classify_domain(domain),
        "fields": {},
        "raw": line,
    }

    if domain == "CDR":
        # body is JSON
        try:
            result["fields"] = json.loads(body)
        except json.JSONDecodeError:
            result["fields"] = {"_raw": body}
    else:
        # body is k=v k=v ...
        for km in KV_RE.finditer(body):
            result["fields"][km.group(1)] = km.group(2)

    return result


def _classify_domain(domain):
    """Map raw domain token to a high-level category."""
    if domain.startswith("CALL_"):
        return "call"
    if domain == "CDR":
        return "cdr"
    if domain == "REGISTER":
        return "register"
    if domain in ("OK", "ERR", "subscribed:"):
        return "cmd"
    return "other"


# ---------------------------------------------------------------------------
# Event correlator: stitch call lifecycle by call_id
# ---------------------------------------------------------------------------

class CallCorrelator:
    """Tracks CALL_INCOMING → CALL_ANSWERED → CALL_END → CDR per call_id.

    Emits a timeline dict when the lifecycle is complete (CDR received).
    """

    def __init__(self, on_complete=None):
        self._calls = OrderedDict()       # call_id -> timeline
        self._on_complete = on_complete   # callback(timeline)
        self._lock = threading.Lock()

    def feed(self, frame):
        if frame is None:
            return None
        fields = frame["fields"]
        call_id = fields.get("call_id") or fields.get("callid")
        domain = frame["domain"]

        with self._lock:
            if call_id is None:
                return None

            tl = self._calls.setdefault(call_id, {
                "call_id": call_id,
                "events": [],
                "cdr": None,
                "start_ts": frame["ts"],
            })

            entry = {
                "ts": frame["ts_str"],
                "domain": domain,
                "fields": fields,
            }
            tl["events"].append(entry)

            if domain == "CDR":
                tl["cdr"] = fields
                tl["end_ts"] = frame["ts"]
                if self._on_complete:
                    self._on_complete(tl)
                return tl

        return None

    def snapshot(self):
        with self._lock:
            return {k: dict(v) for k, v in self._calls.items()}


# ---------------------------------------------------------------------------
# Webhook sender (fire-and-forget per event)
# ---------------------------------------------------------------------------

class WebhookSink:
    """POSTs each parsed frame as JSON to an external URL.

    Designed for call-center integration — every real-time event is forwarded
    to your backend without blocking the event loop.
    """

    def __init__(self, url, timeout_s=5, retries=2):
        self.url = url
        self.timeout_s = timeout_s
        self.retries = retries
        self._ok = 0
        self._fail = 0
        self._lock = threading.Lock()

    def send(self, frame):
        """Non-blocking: spawn a thread to POST."""
        t = threading.Thread(
            target=self._do_post, args=(frame,), daemon=True
        )
        t.start()

    def _do_post(self, frame):
        payload = json.dumps({
            "ts": frame.get("ts_str", ""),
            "domain": frame.get("domain", ""),
            "kind": frame.get("kind", ""),
            "fields": frame.get("fields", {}),
            "raw": frame.get("raw", ""),
        }, default=str).encode("utf-8")

        for attempt in range(self.retries + 1):
            try:
                req = urllib.request.Request(
                    self.url,
                    data=payload,
                    headers={
                        "Content-Type": "application/json",
                        "X-NSBC-Event": frame.get("domain", ""),
                    },
                    method="POST",
                )
                with urllib.request.urlopen(req, timeout=self.timeout_s, context=_ssl_ctx()) as resp:
                    if 200 <= resp.status < 300:
                        with self._lock:
                            self._ok += 1
                        return
            except (urllib.error.URLError, OSError) as e:
                if attempt == self.retries:
                    with self._lock:
                        self._fail += 1
                    print(
                        f'{_c(COLOR["ERR"])}[webhook FAIL] {frame.get("domain")} '
                        f"→ {self.url}: {e}{_c(RESET)}",
                        file=sys.stderr,
                    )
                time.sleep(0.5 * (attempt + 1))

    def stats(self):
        with self._lock:
            return {"ok": self._ok, "fail": self._fail}


# ---------------------------------------------------------------------------
# Pretty printers
# ---------------------------------------------------------------------------

def print_banner(text):
    print(f"{_c(DIM)}{text}{_c(RESET)}")


def print_frame_parsed(frame):
    """Colorized single-line output for a parsed frame."""
    ts = frame.get("ts_str", "")
    domain = frame.get("domain", "")
    fields = frame.get("fields", {})
    color_code = COLOR.get(domain, "\033[37m")

    try:
        short_ts = datetime.strptime(ts[:23], "%Y-%m-%dT%H:%M:%S.%f").strftime(
            "%H:%M:%S.%f"
        )[:-3]
    except (ValueError, TypeError):
        short_ts = ts[:23]

    if domain == "CDR":
        call_id = fields.get("call_id", "?")
        disp = fields.get("disposition", "?")
        dur = fields.get("duration_s", "?")
        bill = fields.get("billsec", "?")
        caller = fields.get("from", "?")
        callee = fields.get("to", "?")
        summary = (
            f"call_id={call_id} {caller}→{callee} "
            f"disp={disp} dur={dur}s bill={bill}s"
        )
        print(f"{short_ts} {_c(color_code):>12s}{_c(RESET)} {summary}")
    else:
        # k=v line: show key fields first if present
        parts = []
        for k in ("call_id", "orig", "to", "src_ip", "trunk", "profile", "hangup_by"):
            if k in fields:
                parts.append(f"{k}={fields[k]}")
        for k, v in fields.items():
            if k not in ("call_id", "orig", "to", "src_ip", "trunk", "profile", "hangup_by"):
                parts.append(f"{k}={v}")
        body = " ".join(parts) if parts else frame.get("raw", "")
        print(f"{short_ts} {_c(color_code)}{domain:>15s}{_c(RESET)} {body}")


def print_timeline(tl):
    """Print a complete call lifecycle timeline."""
    call_id = tl.get("call_id", "?")
    cdr = tl.get("cdr", {})
    events = tl.get("events", [])

    print()
    print(f"{_c(BOLD)}══════ Call Lifecycle: {call_id} ══════{_c(RESET)}")
    for ev in events:
        dom = ev["domain"]
        cc = COLOR.get(dom, "\033[37m")
        print(f"  {_c(cc)}{dom:>15s}{_c(RESET)}  {ev['ts']}")
    if cdr:
        print(f"  {_c(COLOR['CDR']):>15s}{_c(RESET)}  CDR record")
        summary = (
            f"    {cdr.get('from','?')} → {cdr.get('to','?')}  "
            f"disp={cdr.get('disposition','?')}  "
            f"dur={cdr.get('duration_s','?')}s  bill={cdr.get('billsec','?')}s"
        )
        print(summary)
    print(f"{_c(BOLD)}──────────────────────────────────{_c(RESET)}")
    print()


# ---------------------------------------------------------------------------
# Interactive REPL mode
# ---------------------------------------------------------------------------

def run_repl(client):
    """Simple readline-less REPL over UDS socket."""
    import select as _select

    print_banner(client.banner)
    print_banner("[interactive mode — Ctrl-D to exit]")
    client.subscribe(DEFAULT_TOPICS)
    print_banner(f"[subscribed: {DEFAULT_TOPICS}]")

    # Put stdin into non-blocking line-buffered mode
    old_settings = None
    try:
        import termios
        import tty

        old_settings = termios.tcgetattr(sys.stdin)
    except ImportError:
        pass  # Windows — just use select

    try:
        while True:
            r, _, _ = _select.select([sys.stdin, client.sock], [], [])
            if sys.stdin in r:
                ln = sys.stdin.readline()
                if not ln:
                    break
                cmd = ln.rstrip("\n")
                if cmd:
                    if cmd == "quit" or cmd == "exit":
                        break
                    client.sock.sendall((cmd + "\n").encode("utf-8"))
            if client.sock in r:
                chunk = client.sock.recv(65536)
                if not chunk:
                    print("\n[disconnected]", file=sys.stderr)
                    break
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
    finally:
        if old_settings:
            try:
                import termios
                termios.tcsetattr(sys.stdin, termios.TCSADRAIN, old_settings)
            except Exception:
                pass


# ---------------------------------------------------------------------------
# Main orchestration
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Unified frame streaming — abstracts UDS vs SSE transport behind one interface
# ---------------------------------------------------------------------------

def _transport_stream(args):
    """Yield unified parsed frames from either UDS or SSE.

    Returns an iterator of parsed frame dicts (always non-None).
    Handles connect / subscribe / reconnect internally.
    """
    transport = getattr(args, "transport", "auto")

    if transport in ("auto", "uds"):
        # UDS path
        print(f"{_c(DIM)}[connect UDS {args.uds}]{_c(RESET)}")
        try:
            client = UDSClient(args.uds).connect()
        except FileNotFoundError as e:
            if transport == "auto":
                print_banner(f"UDS not available ({e}), falling back to SSE...")
                transport = "sse"
            else:
                print(f"{_c(COLOR['ERR'])}[FAIL] {e}{_c(RESET)}", file=sys.stderr)
                sys.exit(2)
        else:
            print_banner(client.banner.strip())
            topics = args.topics or DEFAULT_TOPICS
            resp = client.subscribe(topics)
            for ln in resp.splitlines():
                if ln.strip():
                    print_banner(f"[sub] {ln.strip()}")
            print_banner(f"[transport=UDS, topics='{topics}' — Ctrl-C to quit]")

            try:
                for raw in client.iter_frames():
                    frame = parse_frame(raw)
                    if frame is None:
                        if raw and not raw.startswith(" "):
                            print_banner(f"[ctrl] {raw}")
                        continue
                    yield frame
            except (BrokenPipeError, ConnectionResetError, OSError) as e:
                print(f"\n{_c(COLOR['ERR'])}[UDS disconnected: {e}]{_c(RESET)}", file=sys.stderr)
            finally:
                client.close()
            return

    # SSE path (explicit or auto-fallback)
    sse_url = getattr(args, "sse_url", None)
    if not sse_url:
        sse_url = args.api_base.rstrip("/") + "/calls/stream"
    print(f"{_c(DIM)}[connect SSE {sse_url}]{_c(RESET)}")

    try:
        sse = SSEClient(sse_url).connect()
    except ConnectionError as e:
        print(f"{_c(COLOR['ERR'])}[FAIL] {e}{_c(RESET)}", file=sys.stderr)
        sys.exit(2)

    print_banner(f"[transport=SSE — Ctrl-C to quit]")

    try:
        for ev in sse.iter_events():
            # SSE lag notifications
            data_str = ev.get("data", "")
            try:
                blob = json.loads(data_str)
                if blob.get("kind") == "lag":
                    print_banner("[sse lag detected — consumer fell behind]")
                    continue
            except json.JSONDecodeError:
                pass
            frame = sse_frame_to_parsed(ev, source="sse")
            if frame is not None:
                yield frame
    except KeyboardInterrupt:
        raise
    finally:
        sse.close()


# ---------------------------------------------------------------------------
# Originate call_url builder  (REQ-ORIGINATE-004: 统一 codec / vars 拼接)
# ---------------------------------------------------------------------------

# 默认 codec 列表（WebRTC 分机能协商的主流 codec）
_DEFAULT_AUDIO_CODECS = "PCMU,PCMA,G722,OPUS"
_DEFAULT_VIDEO_CODECS = "H264,VP8"


def build_call_url(args):
    """从 argparse Namespace 构建完整的 originate call_url。

    优先级：
      1. args.call_url 显式传入 → 原样返回（最高优先级）
      2. 否则拼 FS brace vars + user/<ext> &<app>

    Codec 解析链：
      --video-codecs XXX  → 显式指定，直接用
      --video (无 --video-codecs) → 默认 H264,VP8
      都没传 → 不生成 video m-line

      --audio-codecs XXX → 显式指定
      --audio (无 --audio-codecs) → 默认 PCMU,PCMA,G722,OPUS
      都没传 → 后端默认 PCMU 单 codec（最小 offer）
    """
    if getattr(args, "call_url", None):
        return args.call_url

    ext = args.orig_to or "1000"
    app = args.app or "echo"

    vars_parts = []
    if args.caller_id:
        vars_parts.append(f"origination_caller_id_number={args.caller_id}")
    if args.caller_name:
        vars_parts.append(f"origination_caller_id_name={args.caller_name}")
    if args.timeout:
        vars_parts.append(f"originate_timeout={args.timeout}")

    # ---- REQ-ORIGINATE-004: codec control ----
    audio = args.audio_codecs or (_DEFAULT_AUDIO_CODECS if args.audio else None)
    video = args.video_codecs or (_DEFAULT_VIDEO_CODECS if args.video else None)
    if audio:
        vars_parts.append(f"audio_codecs={audio}")
    if video:
        vars_parts.append(f"video_codecs={video}")

    vars_str = "{" + ",".join(vars_parts) + "}" if vars_parts else ""
    return f"{vars_str}user/{ext} &{app}"


def run_originate_and_subscribe(args):
    """POST /originate, then subscribe events/CDRs (UDS or SSE), correlate and print."""
    # 1. Build call_url
    call_url = build_call_url(args)

    print(f"{_c(DIM)}[1/2] originate via={args.orig_via}  call_url={call_url}{_c(RESET)}")

    ok, payload, used_via = originate(
        call_url, via=args.orig_via, uds_path=args.uds,
        api_base=args.api_base, api_token=args.api_token,
    )
    if not ok:
        if isinstance(payload, dict) and "uds" in payload:
            print(
                f"{_c(COLOR['ERR'])}[FAIL] both UDS and HTTP originate failed{_c(RESET)}",
                file=sys.stderr,
            )
            print(f"  UDS:  {payload['uds']}", file=sys.stderr)
            print(f"  HTTP: {payload['web']}", file=sys.stderr)
        else:
            print(
                f"{_c(COLOR['ERR'])}[FAIL] originate via {used_via}: {payload}{_c(RESET)}",
                file=sys.stderr,
            )
        sys.exit(3)

    if used_via == "uds":
        print(f"{_c(COLOR['OK'])}[OK]{_c(RESET)} originate dispatched via UDS → {payload}")
    else:
        print(f"{_c(COLOR['OK'])}[OK]{_c(RESET)} originate dispatched via HTTP → {payload}")

    # 2. Subscribe (UDS or SSE, abstracted by _transport_stream)
    correlator = CallCorrelator(on_complete=print_timeline)
    webhook = None
    if args.webhook_url:
        webhook = WebhookSink(args.webhook_url)
        print_banner(f"[webhook] every event → POST {args.webhook_url}")

    print_banner("[2/2] subscribing — waiting for events ...")

    try:
        for frame in _transport_stream(args):
            print_frame_parsed(frame)
            correlator.feed(frame)
            if webhook:
                webhook.send(frame)

            # Exit conditions
            if args.exit_on_cdr and frame["domain"] == "CDR":
                time.sleep(0.1)
                break

    except KeyboardInterrupt:
        print(f"\n{_c(DIM)}[Ctrl-C — shutting down]{_c(RESET)}")
    finally:
        if webhook:
            stats = webhook.stats()
            print_banner(f"[webhook stats] ok={stats['ok']} fail={stats['fail']}")
        print_banner("[done]")


def run_subscribe_only(args):
    """Watch events/CDRs/logs in real time (UDS or SSE transport)."""
    correlator = CallCorrelator(on_complete=print_timeline)
    webhook = None
    if args.webhook_url:
        webhook = WebhookSink(args.webhook_url)

    try:
        for frame in _transport_stream(args):
            print_frame_parsed(frame)
            correlator.feed(frame)
            if webhook:
                webhook.send(frame)
    except KeyboardInterrupt:
        print(f"\n{_c(DIM)}[quit]{_c(RESET)}")
    finally:
        if webhook:
            stats = webhook.stats()
            print_banner(f"[webhook stats] ok={stats['ok']} fail={stats['fail']}")


def run_originate_only(args):
    """Fire-and-forget originate — just print the response."""
    call_url = build_call_url(args)

    print(f"{_c(DIM)}originate via={args.orig_via}  call_url={call_url}{_c(RESET)}")

    ok, payload, used_via = originate(
        call_url, via=args.orig_via, uds_path=args.uds,
        api_base=args.api_base, api_token=args.api_token,
    )
    if ok:
        print(f"{_c(COLOR['OK'])}[OK]{_c(RESET)} via={used_via} → {payload}")
    else:
        if isinstance(payload, dict) and "uds" in payload:
            print(f"{_c(COLOR['ERR'])}[FAIL]{_c(RESET)} both UDS+HTTP failed", file=sys.stderr)
            print(f"  UDS:  {payload['uds']}", file=sys.stderr)
            print(f"  HTTP: {payload['web']}", file=sys.stderr)
        else:
            print(f"{_c(COLOR['ERR'])}[FAIL]{_c(RESET)} via={used_via} {payload}", file=sys.stderr)
        sys.exit(3)


def build_argparser():
    ap = argparse.ArgumentParser(
        description="NSBC Event/CDR subscription & originate client (Python, stdlib only)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Default: originate extension 1001 &hangup, observe its lifecycle (UDS → SSE fallback)
  python3 event_client.py

  # Originate extension 2001 &park (hold the call for observation)
  python3 event_client.py --orig-to 2001 --app park

  # Pure observer — watch all events/CDRs via UDS
  python3 event_client.py --mode subscribe --transport uds

  # Pure observer — remote call-center over HTTP SSE (no UDS access needed)
  python3 event_client.py --mode subscribe --transport sse \\
      --sse-url http://nsbc-web.example.com:8080/calls/stream

  # Fire-and-forget originate, no subscription
  python3 event_client.py --mode originate --orig-to 1001

  # Webhook every event to your call-center backend (remote SSE + webhook)
  python3 event_client.py --mode subscribe --transport sse \\
      --sse-url http://nsbc-web.example.com:8080/calls/stream \\
      --webhook-url http://cc.example.com/nsbc/events

  # Interactive UDS REPL (like a minimal nsbc_cli in Python)
  python3 event_client.py --repl
""",
    )
    ap.add_argument(
        "--mode",
        choices=["auto", "originate+subscribe", "subscribe", "originate"],
        default="auto",
        help="Operation mode (default 'auto': auto-detects based on other flags)",
    )
    ap.add_argument("--repl", action="store_true", help="Interactive UDS REPL mode")
    ap.add_argument(
        "--uds", default=DEFAULT_UDS,
        help=f"Path to nsbcd control UDS socket (default: {DEFAULT_UDS})",
    )
    ap.add_argument(
        "--api-base", default=DEFAULT_API_BASE,
        help=f"nsbc-web HTTP API base URL (default: {DEFAULT_API_BASE})",
    )
    ap.add_argument(
        "--topics", default=None,
        help=f"Comma-separated obs_sub topics for UDS (default: {DEFAULT_TOPICS})",
    )
    ap.add_argument(
        "--transport",
        choices=["auto", "uds", "sse"],
        default="auto",
        help="Event stream transport: auto=try UDS fall back SSE, uds=force Unix socket, sse=force HTTP SSE (default: auto)",
    )
    ap.add_argument(
        "--sse-url", default=None,
        help="Full SSE URL for remote transport (default: {api-base}/calls/stream)",
    )
    # Originate params
    ap.add_argument("--orig-via", choices=["auto", "uds", "web"], default="auto",
                    help="originate 触发路径: auto=先试 UDS 回退 HTTP, uds=强制走 nsbcd 控制 socket, web=强制走 nsbc-web /api/v1/originate (default: auto)")
    ap.add_argument("--call-url", default=None,
                    help="Explicit call_url (overrides --orig-to / --app / vars)")
    ap.add_argument("--orig-to", default=None,
                    help="Destination extension/user number (default: 1000)")
    ap.add_argument("--app", default=None,
                    choices=["park", "hangup", "echo"],
                    help="Post-originate app: echo=RTP loopback (hear self), park=hold, hangup=answer+hangup (default: echo)")
    ap.add_argument("--caller-id", default=None, help="Origination caller ID number")
    ap.add_argument("--caller-name", default=None, help="Origination caller ID name")
    ap.add_argument("--timeout", type=int, default=None,
                    help="Ringing timeout in seconds (originate_timeout var)")
    # Media codec control (REQ-ORIGINATE-004)
    # ★ WebRTC 场景默认开视频+多音频 —— 用户不跑 --no-video 就有 H264+VP8
    # 精确控制：--video-codecs / --audio-codecs 显式指定 codec 列表（覆盖默认值）
    ap.add_argument("--video", action="store_true", default=True,
                    help="[默认 True] 开启视频媒体 (默认 codecs: H264,VP8). 等价 --video-codecs H264,VP8")
    ap.add_argument("--no-video", dest="video", action="store_false",
                    help="关闭视频 (纯音频 originate)")
    ap.add_argument("--audio", action="store_true", default=True,
                    help="[默认 True] 开启多音频 codec (默认: PCMU,PCMA,G722,OPUS).")
    ap.add_argument("--no-audio", dest="audio", action="store_false",
                    help="只用单 codec (PCMU) 音频")
    ap.add_argument("--audio-codecs", default=None,
                    help="Comma-separated audio codecs, e.g. PCMU,PCMA,G722 "
                         "(默认: PCMU,PCMA,G722,OPUS). "
                         "Passed as FS brace var {audio_codecs=...}")
    ap.add_argument("--video-codecs", default=None,
                    help="Comma-separated video codecs, e.g. H264,VP8 "
                         "(默认: H264,VP8 -- WebRTC originate 有视频才合理). "
                         "Passed as FS brace var {video_codecs=...}")
    # Auth / TLS
    ap.add_argument("--api-token", default=None,
                    help="Bearer API Key for nsbc-web /ext/v1/* endpoints "
                         "(format: nsbc_live_<32byteshex>, nsbc-web 首次启动时自动生成并打印到日志)")
    ap.add_argument("--insecure", action="store_true",
                    help="Disable TLS certificate verification (self-signed certs OK)")
    # Integration
    ap.add_argument("--webhook-url", default=None,
                    help="POST each event frame as JSON to this URL (call-center integration)")
    ap.add_argument("--exit-on-cdr", action="store_true",
                    help="Exit after receiving the first CDR record")
    return ap


def main():
    args = build_argparser().parse_args()

    # --insecure → 关闭 TLS 证书验证（nsbc-web 自签证书是常态）
    global _SSL_VERIFY
    if args.insecure:
        _SSL_VERIFY = False

    if args.repl:
        client = UDSClient(args.uds).connect()
        run_repl(client)
        client.close()
        return

    # Resolve mode — clean logic
    if args.mode == "auto":
        # Zero-arg default behavior: originate+subscribe (our core value prop)
        # Only switch to pure subscribe when user explicitly says --mode subscribe
        # AND provides no originate params (we handled explicit mode above already).
        args.mode = "originate+subscribe"

    if args.mode == "originate":
        run_originate_only(args)
    elif args.mode == "subscribe":
        run_subscribe_only(args)
    else:  # originate+subscribe
        run_originate_and_subscribe(args)


if __name__ == "__main__":
    main()
