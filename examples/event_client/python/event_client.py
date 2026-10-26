#!/usr/bin/env python3
"""NSBC Event/CDR subscription & originate client (CLI / demo application).

Thin shell — all real API lives in nsbc_client.py (reusable library).
This file is ONLY: argparse + mode dispatch + pretty printing + REPL.

Usage:
    python3 event_client.py                         # default originate 1000 &echo + subscribe
    python3 event_client.py --orig-to 2001 --video  # originate with video (default on)
    python3 event_client.py --mode subscribe        # pure observer over UDS
    python3 event_client.py --mode originate --orig-to 1001 --no-video
    python3 event_client.py --mode subscribe --transport sse \\
        --sse-url http://nsbc-web:8080/calls/stream --webhook-url ...
    python3 event_client.py --repl                  # interactive UDS REPL
"""

import argparse
import json
import os
import select
import sys
import time

from nsbc_client import (
    DEFAULT_UDS, DEFAULT_API_BASE, DEFAULT_TOPICS,
    set_ssl_verify,
    originate, build_call_url,
    NsbcClientUds, NsbcClientSse, sse_event_to_frame,
    parse_frame, CallCorrelator, WebhookSink,
)

# ---------------------------------------------------------------------------
# Terminal colors (disabled when stdout is not a tty)
# ---------------------------------------------------------------------------

_USE_COLOR = sys.stdout.isatty()
COLOR = {
    "CALL_INCOMING": "\033[36m",
    "CALL_ANSWERED": "\033[32m",
    "CALL_END":      "\033[31m",
    "REGISTER":      "\033[33m",
    "CDR":           "\033[35m",
    "OK":            "\033[32m",
    "ERR":           "\033[31m",
}
RESET = "\033[0m"
DIM = "\033[2m"
BOLD = "\033[1m"


def _c(code):
    return code if _USE_COLOR else ""


def print_banner(text):
    print(f"{_c(DIM)}{text}{_c(RESET)}")


def print_frame_parsed(frame):
    """Colorized single-line output for a parsed frame."""
    ts = frame.get("ts_str", "")
    domain = frame.get("domain", "")
    fields = frame.get("fields", {})
    color_code = COLOR.get(domain, "\033[37m")

    short_ts = ts[11:23] if len(ts) >= 23 else ts

    if domain == "CDR":
        summary = (
            f"call_id={fields.get('call_id','?')} "
            f"{fields.get('from','?')}→{fields.get('to','?')} "
            f"disp={fields.get('disposition','?')} "
            f"dur={fields.get('duration_s','?')}s "
            f"bill={fields.get('billsec','?')}s"
        )
        print(f"{short_ts} {_c(color_code):>12s}{_c(RESET)} {summary}")
    else:
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
        print(
            f"    {cdr.get('from','?')} → {cdr.get('to','?')}  "
            f"disp={cdr.get('disposition','?')}  "
            f"dur={cdr.get('duration_s','?')}s  bill={cdr.get('billsec','?')}s"
        )
    print(f"{_c(BOLD)}──────────────────────────────────{_c(RESET)}")
    print()


# ---------------------------------------------------------------------------
# Transport streaming
# ---------------------------------------------------------------------------

def _transport_stream(args):
    """Yield unified parsed frames from UDS or SSE (auto-fallback)."""
    transport = getattr(args, "transport", "auto")

    if transport in ("auto", "uds"):
        print(f"{_c(DIM)}[connect UDS {args.uds}]{_c(RESET)}")
        try:
            client = NsbcClientUds(args.uds).connect()
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

    # SSE path
    sse_url = getattr(args, "sse_url", None) or args.api_base.rstrip("/") + "/calls/stream"
    print(f"{_c(DIM)}[connect SSE {sse_url}]{_c(RESET)}")

    try:
        sse = NsbcClientSse(sse_url).connect()
    except ConnectionError as e:
        print(f"{_c(COLOR['ERR'])}[FAIL] {e}{_c(RESET)}", file=sys.stderr)
        sys.exit(2)

    print_banner("[transport=SSE — Ctrl-C to quit]")

    try:
        for ev in sse.iter_events():
            try:
                blob = json.loads(ev.get("data", ""))
                if blob.get("kind") == "lag":
                    print_banner("[sse lag detected — consumer fell behind]")
                    continue
            except json.JSONDecodeError:
                pass
            frame = sse_event_to_frame(ev)
            if frame is not None:
                yield frame
    except KeyboardInterrupt:
        raise
    finally:
        sse.close()


# ---------------------------------------------------------------------------
# REPL mode
# ---------------------------------------------------------------------------

def run_repl(client):
    """Simple readline-less REPL over UDS socket."""
    print_banner(client.banner)
    print_banner("[interactive mode — Ctrl-D to exit]")
    client.subscribe(DEFAULT_TOPICS)
    print_banner(f"[subscribed: {DEFAULT_TOPICS}]")

    try:
        while True:
            r, _, _ = select.select([sys.stdin, client.sock], [], [])
            if sys.stdin in r:
                ln = sys.stdin.readline()
                if not ln:
                    break
                cmd = ln.rstrip("\n")
                if cmd:
                    if cmd in ("quit", "exit"):
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
        client.close()


# ---------------------------------------------------------------------------
# Mode orchestrations
# ---------------------------------------------------------------------------

def run_originate_and_subscribe(args):
    call_url = build_call_url(
        orig_to=args.orig_to, app=args.app,
        audio=args.audio, video=args.video,
        timeout=args.timeout,
        caller_id=args.caller_id, caller_name=args.caller_name,
        call_url=getattr(args, "call_url", None),
    )

    print(f"{_c(DIM)}[1/2] originate via={args.orig_via}  call_url={call_url}{_c(RESET)}")

    ok, payload, used_via = originate(
        call_url, via=args.orig_via, uds_path=args.uds,
        api_base=args.api_base, api_token=args.api_token,
    )
    if not ok:
        if isinstance(payload, dict) and "uds" in payload:
            print(f"{_c(COLOR['ERR'])}[FAIL] both UDS and HTTP originate failed{_c(RESET)}", file=sys.stderr)
            print(f"  UDS:  {payload['uds']}", file=sys.stderr)
            print(f"  HTTP: {payload['web']}", file=sys.stderr)
        else:
            print(f"{_c(COLOR['ERR'])}[FAIL] originate via {used_via}: {payload}{_c(RESET)}", file=sys.stderr)
        sys.exit(3)

    via_color = COLOR["OK"] if used_via == "uds" else COLOR["OK"]
    print(f"{_c(via_color)}[OK]{_c(RESET)} originate dispatched via {used_via} → {payload}")

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
    call_url = build_call_url(
        orig_to=args.orig_to, app=args.app,
        audio=args.audio, video=args.video,
        timeout=args.timeout,
        caller_id=args.caller_id, caller_name=args.caller_name,
        call_url=getattr(args, "call_url", None),
    )

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


# ---------------------------------------------------------------------------
# CLI entry
# ---------------------------------------------------------------------------

def build_argparser():
    ap = argparse.ArgumentParser(
        description="NSBC Event/CDR subscription & originate client (CLI demo)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 event_client.py                              # originate + subscribe (UDS)
  python3 event_client.py --orig-to 7gja30rf --video   # WebRTC video (default video on)
  python3 event_client.py --mode subscribe             # pure UDS observer
  python3 event_client.py --mode originate --orig-to 1001 --no-video
  python3 event_client.py --mode subscribe --transport sse \\
      --sse-url http://nsbc-web.example.com:8080/calls/stream \\
      --webhook-url http://cc.example.com/nsbc/events
  python3 event_client.py --repl                        # interactive UDS REPL
""",
    )
    ap.add_argument("--mode", choices=["auto", "originate+subscribe", "subscribe", "originate"],
                    default="auto")
    ap.add_argument("--repl", action="store_true", help="Interactive UDS REPL mode")
    ap.add_argument("--uds", default=DEFAULT_UDS,
                    help=f"Path to nsbcd control UDS socket (default: {DEFAULT_UDS})")
    ap.add_argument("--api-base", default=DEFAULT_API_BASE,
                    help=f"nsbc-web HTTP API base URL (default: {DEFAULT_API_BASE})")
    ap.add_argument("--topics", default=None,
                    help=f"obs_sub topics for UDS (default: {DEFAULT_TOPICS})")
    ap.add_argument("--transport", choices=["auto", "uds", "sse"], default="auto",
                    help="Event stream transport: auto/uds/sse (default: auto)")
    ap.add_argument("--sse-url", default=None,
                    help="Full SSE URL (default: {api-base}/calls/stream)")

    # Originate params
    ap.add_argument("--orig-via", choices=["auto", "uds", "web"], default="auto")
    ap.add_argument("--call-url", default=None, help="Explicit call_url (overrides orig-to/app/vars)")
    ap.add_argument("--orig-to", default="1000", help="Destination extension (default: 1000)")
    ap.add_argument("--app", default="echo", choices=["park", "hangup", "echo"],
                    help="Post-originate app (default: echo)")
    ap.add_argument("--caller-id", default=None, help="Origination caller ID number")
    ap.add_argument("--caller-name", default=None, help="Origination caller ID name")
    ap.add_argument("--timeout", type=int, default=None, help="Ringing timeout seconds")

    # Codec control (REQ-ORIGINATE-004 — WebRTC defaults)
    ap.add_argument("--video", dest="video", action="store_true", default=True,
                    help="[default True] enable video (default: H264,VP8)")
    ap.add_argument("--no-video", dest="video", action="store_false",
                    help="disable video (audio-only originate)")
    ap.add_argument("--audio", dest="audio", action="store_true", default=True,
                    help="[default True] multi-audio codecs (default: PCMU,PCMA,G722,OPUS)")
    ap.add_argument("--no-audio", dest="audio", action="store_false",
                    help="single-audio-codec originate")
    ap.add_argument("--audio-codecs", default=None,
                    help="Comma-separated audio codecs (overrides --audio)")
    ap.add_argument("--video-codecs", default=None,
                    help="Comma-separated video codecs (overrides --video)")

    # Auth / TLS
    ap.add_argument("--api-token", default=None,
                    help="Bearer API Key for nsbc-web /ext/v1/* endpoints")
    ap.add_argument("--insecure", action="store_true",
                    help="Disable TLS cert verification (self-signed certs OK)")

    # Integration
    ap.add_argument("--webhook-url", default=None,
                    help="POST each event as JSON to this URL")
    ap.add_argument("--exit-on-cdr", action="store_true",
                    help="Exit after receiving first CDR")
    return ap


def main():
    ap = build_argparser()
    args = ap.parse_args()

    if args.insecure:
        set_ssl_verify(False)

    if args.repl:
        client = NsbcClientUds(args.uds).connect()
        run_repl(client)
        return

    if args.mode == "auto":
        args.mode = "originate+subscribe"

    if args.mode == "originate":
        run_originate_only(args)
    elif args.mode == "subscribe":
        run_subscribe_only(args)
    else:
        run_originate_and_subscribe(args)


if __name__ == "__main__":
    main()
