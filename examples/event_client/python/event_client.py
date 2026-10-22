#!/usr/bin/env python3
"""NSBC 实时事件/CDR 订阅客户端（Unix Domain Socket / Python）

连到 nsbcd 的 control socket（默认 /tmp/nsbc-e2e/control.sock），
先发 sub events,cdr 订阅，然后实时打印推送帧。

用法:
    python3 event_client.py
    python3 event_client.py /var/run/nsbc/control.sock
    python3 event_client.py --topics events,cdr,log
    python3 event_client.py --pull-only

帧格式 (nsbcd 服务端统一):
    events: YYYY-MM-DDTHH:MM:SS.mmm CALL_INCOMING  k=v k=v ...
    events: YYYY-MM-DDTHH:MM:SS.mmm CALL_ANSWERED  k=v k=v ...
    events: YYYY-MM-DDTHH:MM:SS.mmm CALL_END       k=v k=v ...
    cdr:    YYYY-MM-DDTHH:MM:SS.mmm CDR { ... JSON ... }
"""
import argparse, json, os, socket, sys, re, time, selectors
from datetime import datetime

FRAME_RE = re.compile(r'^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3})\s+(\S+)\s+(.*)$')
PROMPT   = 'nsbc> '
BANNER   = 'NSBC '

COLOR = {
    'CALL_INCOMING': '\033[36m',  # cyan
    'CALL_ANSWERED': '\033[32m',  # green
    'CALL_END':      '\033[31m',  # red
    'REGISTER':      '\033[33m',  # yellow
    'CDR':           '\033[35m',  # magenta
}
RESET = '\033[0m'


def color(domain): return COLOR.get(domain, '\033[37m')


def print_frame(line):
    line = line.rstrip('\n\r')
    if not line: return
    m = FRAME_RE.match(line)
    if not m:
        if line.startswith(BANNER):
            print(f'\033[2m[banner] {line}{RESET}')
        elif line.startswith('OK ') or line.startswith('ERR ') or line.startswith('subscribed:'):
            print(f'\033[2m[cmd] {line}{RESET}')
        return
    ts_s, domain, body = m.group(1), m.group(2), m.group(3)
    try:
        ts = datetime.strptime(ts_s, '%Y-%m-%dT%H:%M:%S.%f').strftime('%H:%M:%S.%f')[:-3]
    except ValueError:
        ts = ts_s
    if domain == 'CDR':
        try:
            ev = json.loads(body)
            summary = f"call_id={ev.get('call_id','?')} {ev.get('from','?')}→{ev.get('to','?')} disp={ev.get('disposition','?')} dur={ev.get('duration_s','?')}s bill={ev.get('billsec','?')}s"
        except json.JSONDecodeError:
            summary = body[:120]
        print(f'{ts} {color("CDR"):>12s}{RESET} {summary}')
    else:
        print(f'{ts} {color(domain)}{domain:>15s}{RESET} {body}')


def connect(uds_path):
    if not os.path.exists(uds_path):
        print(f'ERR: UDS socket not found: {uds_path}', file=sys.stderr)
        sys.exit(2)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(30)
    s.connect(uds_path)
    s.settimeout(None)
    chunk = s.recv(4096)
    if chunk:
        text = chunk.decode('utf-8', 'replace')
        print(f'\033[2m[connected to {uds_path}]{RESET}')
        for ln in text.splitlines():
            if ln.startswith(BANNER):
                print(f'\033[2m{ln}{RESET}')
    return s


def recv_until_prompt(s, timeout_s=1.5):
    s.setblocking(False)
    buf = b''
    sel = selectors.DefaultSelector()
    sel.register(s, selectors.EVENT_READ)
    deadline = time.time() + timeout_s
    try:
        while time.time() < deadline:
            for _ in sel.select(timeout=0.2):
                try: chunk = s.recv(65536)
                except BlockingIOError: continue
                if not chunk: break
                buf += chunk
                if b'nsbc> ' in buf: break
            if b'nsbc> ' in buf: break
    finally:
        s.setblocking(True)
        sel.close()
    return buf.decode('utf-8', 'replace')


def main():
    ap = argparse.ArgumentParser(description='NSBC UDS realtime event/cdr client (Python)')
    ap.add_argument('uds', nargs='?', default='/tmp/nsbc-e2e/control.sock')
    ap.add_argument('--topics', default='events,cdr')
    ap.add_argument('--pull-only', action='store_true')
    ap.add_argument('--cmd', action='append', default=[])
    args = ap.parse_args()

    s = connect(args.uds)

    if not args.pull_only:
        s.sendall((f'sub {args.topics}\n').encode())
        resp = recv_until_prompt(s, 2.0)
        for ln in resp.splitlines():
            if ln.strip() and not ln.strip().startswith(PROMPT):
                print(f'\033[2m[sub] {ln.strip()}{RESET}')

    for c in args.cmd:
        s.sendall((c + '\n').encode())
        resp = recv_until_prompt(s, 1.5)
        sys.stdout.write(resp); sys.stdout.flush()

    if args.pull_only:
        print('\033[2m[interactive — Ctrl-D to exit]{RESET}')
        s.sendall(b'\n')
        import select
        while True:
            r, _, _ = select.select([sys.stdin, s], [], [])
            if sys.stdin in r:
                ln = sys.stdin.readline()
                if not ln: break
                s.sendall((ln.rstrip('\n') + '\n').encode())
            if s in r:
                chunk = s.recv(65536)
                if not chunk: break
                sys.stdout.write(chunk.decode('utf-8', 'replace'))
                sys.stdout.flush()
    else:
        print(f'\033[2m[subscribed: {args.topics} — Ctrl-C to quit]{RESET}')
        try:
            for raw in s.makefile('r', buffering=1):
                print_frame(raw)
        except (BrokenPipeError, ConnectionResetError, OSError) as e:
            print(f'\n\033[31m[disconnected: {e}]{RESET}', file=sys.stderr)
        except KeyboardInterrupt:
            print(f'\n\033[2m[quit]{RESET}')
    s.close()


if __name__ == '__main__': main()
