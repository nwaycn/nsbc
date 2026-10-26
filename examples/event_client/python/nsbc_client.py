#!/usr/bin/env python3
"""NSBC Unified Client Library (Python, stdlib only).

Pure library — zero argparse, zero print-to-terminal, zero sys.exit().
Everything this module exposes is reusable by other callers (call-center
integrations, test harnesses, own CLIs).

Separation of concerns:
    THIS FILE  (library)  = connection + command + subscribe + originate +
                            frame parsing + correlator + webhook sink
    event_client.py (app) = argparse + mode dispatch + pretty printing + REPL

Public API (the cross-language contract that Go/Rust/C mirror):

  # Connection
  uds = NsbcClientUds(uds_path).connect()
  sse = NsbcClientSse(url).connect()

  # Commands / subscribe / iter_frames
  uds.send_command("status")                  -> str  (response before prompt)
  uds.subscribe("events,cdr")                 -> str
  uds.iter_frames()                           -> Iterator[Frame]

  # Originate (dual path: UDS preferred → HTTP fallback)
  ok, payload, via = originate(
      call_url, via="auto",
      uds_path=..., api_base=..., api_token=None)
  call_url = build_call_url(
      orig_to="7gja30rf", app="echo",
      audio=["PCMU","PCMA","G722","OPUS"],
      video=["H264","VP8"],
      timeout=None, caller_id=None, caller_name=None)

  # Frame parsing
  frame = parse_frame("2026-10-26T15:30:01.123 CALL_INCOMING  call_id=...")
  # -> Frame{ ts, ts_str, domain, kind, fields:dict, raw }

  # High-level utilities
  correlator = CallCorrelator(on_complete=timeline_cb)
  correlator.feed(frame)

  webhook = WebhookSink("http://cc.example.com/nsbc/events")
  webhook.send(frame)

No third-party deps. Works on Debian 12 / Ubuntu 22.04 out of the box.
"""

import json
import os
import re
import socket
import ssl
import threading
import time
import selectors
import urllib.request
import urllib.error
from datetime import datetime
from collections import OrderedDict


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

DEFAULT_UDS = "/var/run/nsbc/control.sock"
DEFAULT_API_BASE = os.environ.get("NSBC_API_BASE", "https://127.0.0.1:443")
DEFAULT_TOPICS = "events,cdr"
DEFAULT_TIMEOUT = 30

# Frame format: ISO8601-timestamp DOMAIN k=v k=v ...  (single line, \n terminated)
PROMPT = "nsbc> "
BANNER_PREFIX = "NSBC "
FRAME_RE = re.compile(
    r'^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3})\s+(\S+)\s+(.*)$'
)
KV_RE = re.compile(r'(\w+)=(\S+)')

# Default codec lists for REQ-ORIGINATE-004 (WebRTC originate sane defaults)
DEFAULT_AUDIO_CODECS = "PCMU,PCMA,G722,OPUS"
DEFAULT_VIDEO_CODECS = "H264,VP8"


# ---------------------------------------------------------------------------
# SSL / TLS helpers (used by HTTP API and SSE)
# ---------------------------------------------------------------------------

# Per-process global switch — flip via set_ssl_verify(False) before connect
_SSL_VERIFY = True


def set_ssl_verify(verify):
    """Control TLS cert verification for HTTP paths (nsbc-web API + SSE).

    Useful when nsbc-web uses a self-signed cert. Default True (secure).
    """
    global _SSL_VERIFY
    _SSL_VERIFY = bool(verify)


def _ssl_ctx():
    if not _SSL_VERIFY:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        return ctx
    return None  # None = urllib uses system defaults (verify cert)


# ---------------------------------------------------------------------------
# Originate: dual-path architecture
# ---------------------------------------------------------------------------
#
# nsbcd supports two originate paths; originate() picks one or falls back:
#   A. UDS control socket  →  "originate <call_url>\n"
#      (zero-auth, same-host only; UDS 0600 perms == system-level auth)
#   B. nsbc-web HTTP API   →  POST {call_url}  /ext/v1/originate
#      (remote deployments, call-center integrations, need api_token)
#
# Strategy: default via="auto" → try A first; A unavailable/fails → B
#


def originate_via_uds(uds_path, call_url, timeout_s=10):
    """Send 'originate <call_url>' over nsbcd Unix Domain Socket.

    Returns (ok: bool, payload: str) where payload is the raw command response
    (e.g. "OK originate-dispatched call-id=abc123") or an error string.
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
            if ("OK " in text or "ERR " in text) and (
                text.rstrip().endswith(">") or ">" in text[-5:]
            ):
                break
            if len(buf) > 1_000_000:  # safety valve
                break

        s.close()
        text = buf.decode("utf-8", "replace").strip()
        if text.endswith(">"):
            text = text[:-1].strip()
        if text.startswith("OK ") or "\nOK " in text or text == "OK":
            return True, text
        if text.startswith("ERR") or "ERR " in text:
            return False, text
        # No OK/ERR but also no timeout — treat as success (originate is async)
        return True, text if text else "dispatched (async)"

    except (socket.timeout, OSError) as e:
        return False, f"UDS originate failed: {e}"


def originate_via_web(api_base, call_url, timeout_s=10, api_token=None):
    """POST to nsbc-web /ext/v1/originate (REQ-EXT-API-001).

    Returns (ok: bool, payload). Payload is parsed JSON dict on success,
    dict with http_error/detail/network_error keys on failure.
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


def originate(call_url, via="auto", uds_path=DEFAULT_UDS,
              api_base=DEFAULT_API_BASE, api_token=None, timeout_s=10):
    """Unified originate entry point.

    via: "auto" (default) | "uds" | "web"
      auto → try UDS first, HTTP fallback on failure
    Returns (ok: bool, payload, used_via: str)
    """
    if via == "uds":
        ok, payload = originate_via_uds(uds_path, call_url, timeout_s)
        return ok, payload, "uds"
    if via == "web":
        ok, payload = originate_via_web(api_base, call_url, timeout_s, api_token)
        return ok, payload, "web"

    # auto: try UDS first → HTTP fallback
    ok, payload = originate_via_uds(uds_path, call_url, timeout_s)
    if ok:
        return True, payload, "uds"
    ok2, payload2 = originate_via_web(api_base, call_url, timeout_s, api_token)
    if ok2:
        return True, payload2, "web"
    return False, {"uds": payload, "web": payload2}, "both_failed"


def build_call_url(orig_to=None, app=None,
                   audio=None, video=None,
                   timeout=None, caller_id=None, caller_name=None,
                   call_url=None):
    """Build FS-style originate call_url with brace-var codec block.

    If call_url is given, return it verbatim (highest priority).
    Otherwise assemble: {vars}user/<ext> &<app>

    Args:
        orig_to:        destination extension (default "1000")
        app:            post-originate app: "echo" | "park" | "hangup" (default "echo")
        audio:          audio codecs as list or comma-separated str.
                        True (default) → DEFAULT_AUDIO_CODECS
                        None/False → no audio_codecs brace var
        video:          video codecs as list or comma-separated str.
                        True (default) → DEFAULT_VIDEO_CODECS
                        None/False → no video_codecs brace var
        timeout:        ringing timeout seconds
        caller_id:      origination caller ID number
        caller_name:    origination caller ID name
        call_url:       explicit call_url (bypass builder)

    Returns:
        str  e.g. "{audio_codecs=PCMU,PCMA,G722,OPUS,video_codecs=H264,VP8}user/7gja30rf &echo"
    """
    if call_url:
        return call_url

    ext = orig_to or "1000"
    app_name = app or "echo"

    vars_parts = []
    if caller_id:
        vars_parts.append(f"origination_caller_id_number={caller_id}")
    if caller_name:
        vars_parts.append(f"origination_caller_id_name={caller_name}")
    if timeout:
        vars_parts.append(f"originate_timeout={timeout}")

    # --- Audio codecs ---
    if audio is True:
        audio_str = DEFAULT_AUDIO_CODECS
    elif audio:
        audio_str = audio if isinstance(audio, str) else ",".join(audio)
    else:
        audio_str = None
    if audio_str:
        vars_parts.append(f"audio_codecs={audio_str}")

    # --- Video codecs ---
    if video is True:
        video_str = DEFAULT_VIDEO_CODECS
    elif video:
        video_str = video if isinstance(video, str) else ",".join(video)
    else:
        video_str = None
    if video_str:
        vars_parts.append(f"video_codecs={video_str}")

    vars_str = "{" + ",".join(vars_parts) + "}" if vars_parts else ""
    return f"{vars_str}user/{ext} &{app_name}"


# ---------------------------------------------------------------------------
# Transport: UDSClient (Unix Domain Socket obs_sub)
# ---------------------------------------------------------------------------

class NsbcClientUds:
    """Thin wrapper around nsbcd Unix Domain Socket control interface.

    Usage:
        uds = NsbcClientUds("/var/run/nsbc/control.sock").connect()
        uds.subscribe("events,cdr")
        for frame in uds.iter_frames():
            ...
        uds.close()

    Threading: send_command() is protected by an internal lock so multiple
    threads can send commands concurrently. iter_frames() is a blocking
    generator that should run on its own thread.
    """

    def __init__(self, uds_path, connect_timeout_s=30):
        self.uds_path = uds_path
        self.sock = None
        self._banner = ""
        self._lock = threading.Lock()
        self._connect_timeout_s = connect_timeout_s

    def connect(self):
        if not os.path.exists(self.uds_path):
            raise FileNotFoundError(
                f"UDS socket not found: {self.uds_path}\n"
                f"  Did nsbcd start? Check config 'uds_control.path'."
            )
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(self._connect_timeout_s)
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

        Returns the raw response string (without trailing prompt).
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
        if text.endswith(PROMPT):
            text = text[: -len(PROMPT)]
        elif PROMPT in text:
            text = text[: text.rindex(PROMPT)]
        return text

    def subscribe(self, topics):
        """Send sub command, return server response."""
        return self.send_command(f"sub {topics}")

    def unsubscribe(self, topics=None):
        """Send unsub command. topics=None → unsubscribe all."""
        arg = topics if topics else ""
        return self.send_command(f"unsub {arg}".rstrip())

    def iter_frames(self):
        """Yield raw frame lines pushed by nsbcd.

        Each yield is one complete frame (rstripped of \n\r).
        Blocks until connection closes.
        """
        for raw in self.sock.makefile("r", buffering=1):
            line = raw.rstrip("\n\r")
            if line:
                yield line


# ---------------------------------------------------------------------------
# Transport: SSEClient (Server-Sent Events over HTTP — remote deployments)
# ---------------------------------------------------------------------------

class NsbcClientSse:
    """Minimal SSE client consuming nsbc-web /calls/stream.

    Each SSE event: { "event": str, "data": str, "retry": int|None }
    Blocking reconnect loop is in iter_events().
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
        # Content-Type check is best-effort — some proxies strip it
        return self

    def close(self):
        if self._resp:
            try:
                self._resp.close()
            except Exception:
                pass
            self._resp = None

    def iter_events(self):
        """Yield parsed SSE event dicts forever. Auto-reconnects on disconnect."""
        while True:
            try:
                for ev in self._read_stream():
                    yield ev
                # Stream ended
                time.sleep(2)
                self.connect()
            except (ConnectionError, OSError):
                time.sleep(3)
                try:
                    self.connect()
                except Exception:
                    pass

    def _read_stream(self):
        reader = self._resp
        event_name = "message"
        data_lines = []
        retry_ms = None

        while True:
            try:
                chunk = reader.readline()
                if not chunk:
                    break
            except (socket.timeout, TimeoutError, OSError):
                break

            line = chunk.decode("utf-8", "replace").rstrip("\n\r")

            if line == "":
                if data_lines or event_name != "message":
                    yield {
                        "event": event_name,
                        "data": "\n".join(data_lines),
                        "retry": retry_ms,
                    }
                event_name = "message"
                data_lines = []
                retry_ms = None
                continue

            if line.startswith(":"):
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


def sse_event_to_frame(sse_event):
    """Convert SSE event dict → unified Frame dict (same shape as UDS parse_frame).

    nsbc-web /calls/stream emits SSE event=session where payload.event is
    "ring" / "answer" / "hangup" / "originate" — we map these to the same
    uppercase 'domain' tokens that UDS obs_sub frames use.
    """
    try:
        blob = json.loads(sse_event.get("data", ""))
    except json.JSONDecodeError:
        return None

    kind_outer = blob.get("kind", "unknown")
    payload = blob.get("payload", {})
    if not isinstance(payload, dict):
        return None

    SSE_TO_DOMAIN = {
        "ring": "CALL_INCOMING",
        "answer": "CALL_ANSWERED",
        "hangup": "CALL_END",
        "originate": "CALL_OUTGOING",
    }
    sse_evt = (payload.get("event") or "").lower()
    domain = SSE_TO_DOMAIN.get(sse_evt, sse_evt.upper() or kind_outer.upper())

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
        "_source": "sse",
    }


# ---------------------------------------------------------------------------
# Frame parsing (single obs_sub line → structured dict)
# ---------------------------------------------------------------------------

def parse_frame(line):
    """Parse one obs_sub push frame.

    Returns dict: { ts, ts_str, domain, kind, fields, raw }
    Returns None if the line is not a recognized push frame.
    """
    line = (line or "").rstrip("\n\r")
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
    """Map raw domain token → high-level category (internal, stable strings)."""
    if domain.startswith("CALL_") or domain == "CALL_OUTGOING":
        return "call"
    if domain == "CDR":
        return "cdr"
    if domain == "REGISTER":
        return "register"
    if domain in ("OK", "ERR", "subscribed:"):
        return "cmd"
    return "other"


# ---------------------------------------------------------------------------
# CallCorrelator: stitch call lifecycle by call_id
# ---------------------------------------------------------------------------

class CallCorrelator:
    """Tracks CALL_INCOMING → CALL_ANSWERED → CALL_END → CDR per call_id.

    Emits a complete timeline dict via on_complete callback when the CDR
    arrives (end-of-life). Thread-safe — feed() can be called from any thread.

    Usage:
        correlator = CallCorrelator(on_complete=print_timeline)
        for frame in uds.iter_frames():
            correlator.feed(parse_frame(frame))
    """

    def __init__(self, on_complete=None):
        self._calls = OrderedDict()
        self._on_complete = on_complete
        self._lock = threading.Lock()

    def feed(self, frame):
        if frame is None:
            return None
        fields = frame.get("fields", {})
        call_id = fields.get("call_id") or fields.get("callid")
        domain = frame.get("domain", "")

        with self._lock:
            if call_id is None:
                return None

            tl = self._calls.setdefault(call_id, {
                "call_id": call_id,
                "events": [],
                "cdr": None,
                "start_ts": frame.get("ts"),
            })

            tl["events"].append({
                "ts": frame.get("ts_str", ""),
                "domain": domain,
                "fields": fields,
            })

            if domain == "CDR":
                tl["cdr"] = fields
                tl["end_ts"] = frame.get("ts")
                if self._on_complete:
                    self._on_complete(tl)
                return tl
        return None

    def snapshot(self):
        with self._lock:
            return {k: dict(v) for k, v in self._calls.items()}

    def clear(self):
        with self._lock:
            self._calls.clear()


# ---------------------------------------------------------------------------
# WebhookSink: fire-and-forget forward of every event to a call-center backend
# ---------------------------------------------------------------------------

class WebhookSink:
    """POSTs each parsed frame as JSON to an external URL.

    Designed for call-center integrations. Each POST runs in a daemon thread
    so the event loop is never blocked. Thread-safe stats.
    """

    def __init__(self, url, timeout_s=5, retries=2):
        self.url = url
        self.timeout_s = timeout_s
        self.retries = retries
        self._ok = 0
        self._fail = 0
        self._lock = threading.Lock()

    def send(self, frame):
        """Non-blocking POST — spawns a daemon thread."""
        threading.Thread(
            target=self._do_post, args=(frame,), daemon=True
        ).start()

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
            except (urllib.error.URLError, OSError):
                if attempt == self.retries:
                    with self._lock:
                        self._fail += 1
                time.sleep(0.5 * (attempt + 1))

    def stats(self):
        with self._lock:
            return {"ok": self._ok, "fail": self._fail}
