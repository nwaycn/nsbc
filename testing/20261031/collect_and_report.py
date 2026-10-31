#!/usr/bin/env python3
"""Collect remote results + generate report"""
import paramiko, json, os, time

s = paramiko.SSHClient()
s.set_missing_host_key_policy(paramiko.AutoAddPolicy())
s.connect('10.0.0.80', port=22322, username='root', password='Nway@2025', timeout=15)

local_out = os.path.dirname(os.path.abspath(__file__))

# 1. SFTP pull
print("[SFTP] 拉回远程归档...")
sftp = s.open_sftp()
remote = "/tmp/nsbc_matrix_20261031"
for f in sftp.listdir(remote):
    rp = f"{remote}/{f}"
    lp = os.path.join(local_out, f)
    sz = sftp.stat(rp).st_size
    print(f"  ↓ {f} ({sz:,} bytes)")
    try:
        sftp.get(rp, lp)
    except Exception as e:
        print(f"    !! {e}")
sftp.close()

# 2. 拉 nsbcd 完整日志（tail 1000）
print("[SSH] 拉 nsbcd.log tail...")
_, o, _ = s.exec_command("tail -2000 /tmp/bcd.log 2>/dev/null | grep -iE 'crash|panic|segfault|assert|fatal|abort|deadlock|overflow|ring|busy_timeout|db_locked|EADDRINUSE' | tail -50")
critical_log = o.read().decode()
with open(os.path.join(local_out, "nsbcd_critical.log"), "w", encoding="utf-8") as f:
    f.write(critical_log)
print(f"  ✅ {len(critical_log.splitlines())} lines")

# 3. 拉最终 DB 状态
print("[SSH] 最终 DB 状态...")
_, o2, _ = s.exec_command(r'''
echo "calls=$(sqlite3 /home/github/nsbc/db/web.db 'SELECT count(*) FROM calls;')"
echo "cdr=$(sqlite3 /home/github/nsbc/db/web.db 'SELECT count(*) FROM cdr;')"
echo "registrations=$(sqlite3 /home/github/nsbc/db/web.db 'SELECT count(*) FROM registrations;')"
echo ""
echo "nsbcd stats (ring/calls/cdr):"
python3 -c "
import socket,time
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.connect('/var/run/nsbc/control.sock'); s.sendall(b'stats\n')
time.sleep(0.5); d=s.recv(65535).decode(); s.close()
for line in d.split('\n'):
    if any(k in line.lower() for k in ['calls','cdr','uds','ring','memory','worker']):
        print(line)
" 2>/dev/null | grep -v "^#" | grep -v "^$"
echo ""
echo "memory:"
pid=$(pgrep nsbcd)
cat /proc/$pid/status 2>/dev/null | grep -E 'Vm|Threads'
''')
db_state = o2.read().decode()
with open(os.path.join(local_out, "db_state.txt"), "w", encoding="utf-8") as f:
    f.write(db_state)
print(db_state[:500])

s.close()
print("\n✅ 所有文件已拉回 →", local_out)

# 4. 生成 summary.json
results_json = os.path.join(local_out, "results.json")
records = []
with open(results_json, encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if not line: continue
        try: records.append(json.loads(line))
        except: pass

with open(os.path.join(local_out, "summary.json"), "w", encoding="utf-8") as f:
    json.dump(records, f, ensure_ascii=False, indent=2)
print(f"✅ summary.json ({len(records)} records)")

# 5. 计算真实指标
print("\n" + "="*80)
print("  矩阵结果分析（基于 CDR 增量）")
print("="*80)
print(f"{'Tag':<18}{'Mode':<6}{'CPS':>5}{'Wall':>6}{'CDR Δ':>7}{'RSS kB':>12}{'RingΔ':>7}{'Alive':>6}{'实际CPS':>8}")
print("-"*80)
for r in records:
    tag = r["tag"]
    mode = r["mode"]
    cps = r["cps"]
    wall = r.get("wall_time", 0) or 0
    cdr_delta = r["cdr_after"] - r["cdr_before"]
    rss_delta = f"{r['vmrss_before']}→{r['vmrss_after']}"
    ring_delta = r["ring_after"] - r["ring_before"]
    alive = r.get("alive", "?")
    # 实际 CPS = CDR 增量 / min(wall, 15 秒实际呼叫时间)
    # 但 wall 包含 timeout kill 时间，用 duration=15s 作为实际呼叫时间
    actual_cps = round(cdr_delta / 15, 1) if wall > 0 else 0
    print(f"{tag:<18}{mode:<6}{str(cps):>5}{str(wall):>6}{cdr_delta:>7}{rss_delta:>12}{ring_delta:>7}{alive:>6}{actual_cps:>8}")

# 分组统计
bypass = [r for r in records if r["mode"]=="BYPASS" and r.get("alive")=="✅"]
relay = [r for r in records if r["mode"]=="RELAY" and r.get("alive")=="✅"]
print(f"\nBYPASS: {len(bypass)} 档存活, 最高目标 CPS={max(r['cps'] for r in bypass) if bypass else '?'}, 最高内存={max(r['vmrss_after'] for r in bypass) if bypass else '?'} kB")
print(f"RELAY:  {len(relay)} 档存活, 最高目标 CPS={max(r['cps'] for r in relay) if relay else '?'}, 最高内存={max(r['vmrss_after'] for r in relay) if relay else '?'}")

# ring_dropped
max_ring = max((r["ring_after"] for r in records), default=0)
print(f"\nUDS ring_dropped max = {max_ring} (0 = 无溢出 ✅)")

# crash 统计
crashed = [r for r in records if r.get("alive") != "✅"]
skipped = [r for r in records if r.get("status") == "skipped"]
print(f"Crash 档数: {len(crashed)}, Skip 档数: {len(skipped)}")
