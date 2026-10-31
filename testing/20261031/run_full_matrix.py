#!/usr/bin/env python3
"""
NSBC Phase 2 完整 SIP 压测矩阵 — paramiko 一键执行
远程机: 10.0.0.80:22322 (Debian 12, 16C/16GB)
矩阵:
  BYPASS CPS: 30, 100, 300, 800, 1200, 1600
  RELAY CPS:  30, 50, 100, 200, 300
容错: 每档检查 nsbcd 存活，crash 标 ❌ 继续下一档
归档: /tmp/nsbc_matrix_20261031/ (远程) → 拉回本地 testing/20261031/
"""
import paramiko, json, time, os, sys
from datetime import datetime

REMOTE_HOST = "10.0.0.80"
REMOTE_PORT = 22322
REMOTE_USER = "root"
REMOTE_PASS = "Nway@2025"
LOCAL_OUT = os.path.dirname(os.path.abspath(__file__))

def make_bash():
    return r'''
set +e
cd /home/github/nsbc || { echo "❌ 找不到 /home/github/nsbc"; exit 1; }

ARCHIVE=/tmp/nsbc_matrix_20261031
mkdir -p $ARCHIVE
RESULTS_JSON=$ARCHIVE/results.json
MASTER_LOG=$ARCHIVE/master.log
rm -f $RESULTS_JSON $MASTER_LOG

log() { echo "$@" >> "$MASTER_LOG"; }

log "============================================================"
log "  NSBC Phase 2 完整 SIP 压测矩阵"
log "  $(date)"
log "============================================================"

# ==================== [1/9] 全杀 + 清理 ====================
log ""; log "▶ [1/9] 全杀 + 清理 $(date)"
pkill -9 -f stress_invite 2>/dev/null
pkill -9 -f uas.py 2>/dev/null
pkill -9 -f sipp 2>/dev/null
sleep 2
# 不 kill nsbcd/nsbc-web — 后面会完整重启

# ==================== [2/9] 配置修正（ratelimit/dialplan/external_ip） ====================
log ""; log "▶ [2/9] 配置修正 $(date)"
# ratelimit → 99999 (压测必须)
sed -i 's/ratelimit_regs: 10/ratelimit_regs: 99999/g' conf/nsbc.yaml
sed -i 's/ratelimit_invites: 20/ratelimit_invites: 99999/g' conf/nsbc.yaml
sed -i 's/ratelimit_invites_active: 5/ratelimit_invites_active: 99999/g' conf/nsbc.yaml
log "  ratelimit (sed 后):"
grep -E 'ratelimit_(regs|invites)' conf/nsbc.yaml | while read l; do log "    $l"; done

# DB 修正
sqlite3 db/web.db "UPDATE sip_profiles SET external_sip_ip='10.0.0.80', external_rtp_ip='10.0.0.80' WHERE 1;"
sqlite3 db/web.db "UPDATE dialplan_rules SET action_json='{}' WHERE layer='ext' AND enabled=1;"
log "  dialplan (BYPASS 默认):"
sqlite3 db/web.db "SELECT name, action_json FROM dialplan_rules WHERE enabled=1;" | while read l; do log "    $l"; done

# ==================== [3/9] 完整重启（ratelimit 改 conf 必须重启 nsbcd） ====================
log ""; log "▶ [3/9] 完整重启 nsbc-web → nsbcd $(date)"
pkill -9 -f nsbcd 2>/dev/null
pkill -9 -f nsbc-web 2>/dev/null
sleep 3
rm -f /var/run/nsbc/*.sock /var/run/nsbc/*.pid

# nsbc-web 先启（创建 data.sock）
nohup ./nsbc-web > /tmp/web.log 2>&1 &
sleep 25
if [ -S /var/run/nsbc/data.sock ]; then
  log "  ✅ data.sock OK"
else
  log "  ❌ data.sock not found — tail web.log:"
  tail -20 /tmp/web.log >> "$MASTER_LOG" 2>&1
  exit 1
fi

# nsbcd 启（⚠️ -c conf/nsbc.yaml）
nohup bash -c 'ulimit -c unlimited && exec ./build/nsbcd -c conf/nsbc.yaml' > /tmp/bcd.log 2>&1 &
sleep 20
if pgrep nsbcd >/dev/null 2>&1; then
  log "  ✅ nsbcd PID: $(pgrep nsbcd)"
else
  log "  ❌ nsbcd 未启动 — tail bcd.log:"
  tail -20 /tmp/bcd.log >> "$MASTER_LOG" 2>&1
  exit 1
fi

# 验证
log "  control.sock: $(ls -la /var/run/nsbc/control.sock 2>/dev/null | awk '{print $5}')"
log "  nsbc-web PID: $(pgrep nsbc-web)"
log "  SIP 5060: $(ss -ulnp 2>/dev/null | grep 5060 | awk '{print $7}')"
# 用 nsbcd 的 reload 验证 ratelimit 新值（reload 读 YAML）
python3 -c "import socket; s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.connect('/var/run/nsbc/control.sock'); s.sendall(b'reload\n'); print('reload:', s.recv(4096)); s.close()" >> "$MASTER_LOG" 2>&1

# ==================== [4/9] 启 UAS + sanity ====================
log ""; log "▶ [4/9] 启 UAS + sanity $(date)"
UAS_PATH="testing/reports/20261030-sip-stress-matrix/uas.py"
STRESS_PATH="testing/reports/20261030-sip-stress-matrix/stress_invite.py"
nohup python3 $UAS_PATH --port 5058 --hangup-ms 500 > /tmp/uas.log 2>&1 &
sleep 3
log "  UAS PID: $(pgrep -a uas.py | head -1)"

# sanity
log "  --- SANITY 3 CPS ---"
timeout 60 python3 $STRESS_PATH \
  --cps 3 --duration 5 --workers 2 --users 6 --user-start 10100 \
  > $ARCHIVE/sanity.log 2>&1
ALIVE=$(pgrep nsbcd 2>/dev/null && echo '✅' || echo '❌')
log "  nsbcd alive: $ALIVE"
[ "$ALIVE" = "❌" ] && { log "❌ Sanity crash — 退出"; exit 1; }

# 预注册一批用户（第一档不带 --skip-register，注册完顺带呼叫没关系）
log "  --- 预注册 200 users (cps=5, dur=10) ---"
timeout 120 python3 $STRESS_PATH \
  --cps 5 --duration 10 --workers 4 --users 200 --user-start 10200 \
  > $ARCHIVE/pre_register.log 2>&1
sqlite3 db/web.db "SELECT count(*) FROM registrations;" >> "$MASTER_LOG" 2>&1

# ==================== [5/9] BYPASS 完整矩阵 ====================
log ""; log "▶ [5/9] BYPASS 完整矩阵 $(date)"

# run_single_test: mode cps duration workers users user_start skip_register
run_single_test() {
  local mode=$1 cps=$2 duration=$3 workers=$4 users=$5 user_start=$6 skip_reg=$7
  local tag="${mode}_C${cps}"
  local logfile="$ARCHIVE/${tag}.log"

  log ""
  log "  --- ${tag} dur=${duration}s w=${workers} u=${users} --- $(date)"

  # 容错检查
  if ! pgrep nsbcd >/dev/null 2>&1; then
    log "  ❌ nsbcd not alive — skip ${tag}"
    echo '{"tag":"'${tag}'","mode":"'${mode}'","cps":'${cps}',"status":"skipped","reason":"nsbcd_not_alive"}' >> $RESULTS_JSON
    return 1
  fi

  # 前态采集
  local pid=$(pgrep nsbcd)
  local vmrss_b=$(cat /proc/$pid/status 2>/dev/null | awk '/VmRSS/{print $2}')
  local thr_b=$(cat /proc/$pid/status 2>/dev/null | awk '/Threads/{print $2}')
  local cdr_b=$(sqlite3 db/web.db "SELECT count(*) FROM cdr;" 2>/dev/null)
  local ring_b=$(python3 -c "
import socket,time
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.connect('/var/run/nsbc/control.sock'); s.sendall(b'stats\n')
time.sleep(0.3); d=s.recv(65535).decode(); s.close()
for line in d.split('\n'):
    if 'uds_ring_dropped' in line and 'TYPE' not in line and 'HELP' not in line: print(line.split()[-1])
" 2>/dev/null | tail -1)
  [ -z "$ring_b" ] && ring_b=0
  [ -z "$vmrss_b" ] && vmrss_b=0
  [ -z "$thr_b" ] && thr_b=0
  [ -z "$cdr_b" ] && cdr_b=0
  log "  前态: VmRSS=${vmrss_b}kB Threads=${thr_b} CDR=${cdr_b} ring_dropped=${ring_b}"

  # 执行压测
  local skip_flag=""
  [ "$skip_reg" = "1" ] && skip_flag="--skip-register"
  local t0=$(date +%s)
  timeout 60 python3 $STRESS_PATH \
    --cps $cps --duration $duration --workers $workers --users $users --user-start $user_start \
    $skip_flag > $logfile 2>&1
  local rc=$?
  local t1=$(date +%s)
  local wall=$((t1 - t0))

  # 后态
  local alive="✅"; local vmrss_a=0; local thr_a=0; local cdr_a=0; local ring_a=0
  if ! pgrep nsbcd >/dev/null 2>&1; then
    alive="❌ CRASHED"
  else
    local pid2=$(pgrep nsbcd)
    vmrss_a=$(cat /proc/$pid2/status 2>/dev/null | awk '/VmRSS/{print $2}')
    thr_a=$(cat /proc/$pid2/status 2>/dev/null | awk '/Threads/{print $2}')
    cdr_a=$(sqlite3 db/web.db "SELECT count(*) FROM cdr;" 2>/dev/null)
    ring_a=$(python3 -c "
import socket,time
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.connect('/var/run/nsbc/control.sock'); s.sendall(b'stats\n')
time.sleep(0.3); d=s.recv(65535).decode(); s.close()
for line in d.split('\n'):
    if 'uds_ring_dropped' in line and 'TYPE' not in line and 'HELP' not in line: print(line.split()[-1])
" 2>/dev/null | tail -1)
    [ -z "$vmrss_a" ] && vmrss_a=0
    [ -z "$thr_a" ] && thr_a=0
    [ -z "$cdr_a" ] && cdr_a=0
    [ -z "$ring_a" ] && ring_a=0
  fi

  # 解析结果
  local succ="N/A" total=0 timeout_cnt=0
  [ -f "$logfile" ] && {
    succ=$(grep -oP 'Success rate:\s*\K[\d.]+' $logfile 2>/dev/null | tail -1)
    total=$(grep -oP 'Total:\s*\K\d+' $logfile 2>/dev/null | tail -1)
    timeout_cnt=$(grep -oP 'Timeout:\s*\K\d+' $logfile 2>/dev/null | tail -1)
  }
  [ -z "$succ" ] && succ="N/A"
  [ -z "$total" ] && total=0
  [ -z "$timeout_cnt" ] && timeout_cnt=0

  log "  Wall=${wall}s | Success=${succ}% | Total=${total} | Timeout=${timeout_cnt} | alive=${alive}"
  log "  后态: VmRSS=${vmrss_a}kB Threads=${thr_a} CDR=${cdr_a} ring_dropped=${ring_a}"

  # JSON 记录（用 echo 单行, 不用 cat here-doc — 避免 SIGPIPE 残留）
  local cdr_delta=$((cdr_a - cdr_b))
  echo "{\"tag\":\"${tag}\",\"mode\":\"${mode}\",\"cps\":${cps},\"duration\":${duration},\"workers\":${workers},\"users\":${users},\"user_start\":${user_start},\"skip_register\":${skip_reg},\"wall_time\":${wall},\"rc\":${rc},\"success_rate\":\"${succ}\",\"total_calls\":${total},\"timeout_count\":${timeout_cnt},\"alive\":\"${alive}\",\"vmrss_before\":${vmrss_b},\"vmrss_after\":${vmrss_a},\"threads_before\":${thr_b},\"threads_after\":${thr_a},\"cdr_before\":${cdr_b},\"cdr_after\":${cdr_a},\"cdr_delta\":${cdr_delta},\"ring_before\":${ring_b},\"ring_after\":${ring_a}}" >> $RESULTS_JSON

  [ "$alive" != "✅" ] && {
    log "  ⚠️ crash — core dump:"
    ls -lt /home/github/nsbc/core* 2>/dev/null | head -3 || log "    无"
  }
  sleep 2
}

# ===== BYPASS 矩阵：CPS 30/100/300/800/1200/1600 =====
BYPASS_CPS_LIST="30 100 300 800 1200 1600"
USER_BASE=11000
for cps in $BYPASS_CPS_LIST; do
  # Debian spawn 模式 workers 多了 join 超慢（坑 W2e），保守上限 4
  workers=1
  [ $cps -ge 100 ] && workers=2
  [ $cps -ge 500 ] && workers=4
  users=$((cps * 15))
  user_start=$((USER_BASE + cps * 100))
  run_single_test "BYPASS" $cps 15 $workers $users $user_start 1
done

# ==================== [6/9] 切 RELAY ====================
log ""; log "▶ [6/9] 切 RELAY $(date)"
if ! pgrep nsbcd >/dev/null 2>&1; then
  log "⚠️ BYPASS 后 nsbcd crash — 重启继续 RELAY"
  nohup bash -c 'ulimit -c unlimited && exec ./build/nsbcd -c conf/nsbc.yaml' > /tmp/bcd.log 2>&1 &
  sleep 20
fi
sqlite3 db/web.db "UPDATE dialplan_rules SET action_json='{\"media_mode\":2}' WHERE layer='ext' AND enabled=1;"
python3 -c "import socket; s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.connect('/var/run/nsbc/control.sock'); s.sendall(b'reload\n'); print('reload:', s.recv(4096)); s.close()" >> "$MASTER_LOG" 2>&1
sleep 3
log "  dialplan RELAY:"
sqlite3 db/web.db "SELECT name, action_json FROM dialplan_rules WHERE enabled=1;" | while read l; do log "    $l"; done

# ==================== [7/9] RELAY sanity + 矩阵 ====================
log ""; log "▶ [7/9] RELAY sanity $(date)"
log "  --- RELAY SANITY 3 CPS ---"
timeout 60 python3 $STRESS_PATH \
  --cps 3 --duration 5 --workers 2 --users 6 --user-start 20100 \
  > $ARCHIVE/relay_sanity.log 2>&1
log "  nsbcd alive: $(pgrep nsbcd 2>/dev/null && echo '✅' || echo '❌')"

log ""; log "▶ [7b/9] RELAY 完整矩阵 $(date)"
RELAY_CPS_LIST="30 50 100 200 300"
USER_BASE=21000
for cps in $RELAY_CPS_LIST; do
  workers=1
  [ $cps -ge 50 ] && workers=2
  [ $cps -ge 200 ] && workers=4
  users=$((cps * 15))
  user_start=$((USER_BASE + cps * 100))
  run_single_test "RELAY" $cps 15 $workers $users $user_start 1
done

# ==================== [8/9] 最终状态 ====================
log ""; log "▶ [8/9] 最终状态 $(date)"
log "  nsbcd: $(pgrep -a nsbcd | head -1 || echo '❌ CRASHED')"
log "  nsbc-web: $(pgrep -a nsbc-web | head -1 || echo '❌')"
log "  cores:"
ls -lt /home/github/nsbc/core* 2>/dev/null | head -5 | while read l; do log "    $l"; done || log "    无"
if pgrep nsbcd >/dev/null; then
  pid=$(pgrep nsbcd)
  log "  内存:"
  cat /proc/$pid/status 2>/dev/null | grep -E 'Vm|Threads' | while read l; do log "    $l"; done
  log "  ring_dropped:"
  python3 -c "
import socket,time
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.connect('/var/run/nsbc/control.sock'); s.sendall(b'stats\n')
time.sleep(0.5); d=s.recv(65535).decode(); s.close()
for line in d.split('\n'):
    if any(k in line.lower() for k in ['call','cdr','uds','ring']): print(line)
" 2>/dev/null | while read l; do log "    $l"; done
fi
log "  CDR 总数: $(sqlite3 db/web.db 'SELECT count(*) FROM cdr;')"

# ==================== [9/9] 归档 ====================
log ""; log "▶ [9/9] 归档 $(date)"
cd $ARCHIVE
tar czf nsbc_matrix_20261031.tar.gz *.log results.json master.log 2>/dev/null
log "  ✅ $ARCHIVE/nsbc_matrix_20261031.tar.gz ($(ls -la nsbc_matrix_20261031.tar.gz | awk '{print $5}') bytes)"
log "============================================================"
log "  全部完成 $(date)"
log "============================================================"
'''


def run_remote(ssh, script, timeout=2400):
    print(f"[SSH] 执行完整压测脚本 (timeout={timeout}s)...")
    stdin, stdout, stderr = ssh.exec_command("bash", timeout=timeout)
    stdin.write(script)
    stdin.channel.shutdown_write()
    
    output = ""
    start = time.time()
    while True:
        line = stdout.readline()
        if not line:
            break
        output += line
        elapsed = int(time.time() - start)
        # 实时输出，带 elapsed
        if '▶' in line or '---' in line or '❌' in line or '✅' in line or 'Wall=' in line:
            print(f"  [{elapsed}s] {line.rstrip()}")
    
    err = stderr.read().decode()
    if err.strip() and 'TERM' not in err:
        print(f"[STDERR] {err.strip()[-1000:]}")
    return output


def collect(ssh, local_out):
    print(f"\n[SFTP] 拉回结果 → {local_out}")
    sftp = ssh.open_sftp()
    remote = "/tmp/nsbc_matrix_20261031"
    os.makedirs(local_out, exist_ok=True)
    try:
        for f in sftp.listdir(remote):
            rp = f"{remote}/{f}"
            lp = os.path.join(local_out, f)
            sz = sftp.stat(rp).st_size
            print(f"  ↓ {f} ({sz:,} bytes)")
            sftp.get(rp, lp)
    except Exception as e:
        print(f"  ❌ SFTP: {e}")
    sftp.close()
    print("  ✅ 完成")


def parse_summary(local_out):
    jpath = os.path.join(local_out, "results.json")
    if not os.path.exists(jpath):
        print(f"❌ 无 results.json")
        return []
    records = []
    with open(jpath, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line: continue
            try: records.append(json.loads(line))
            except: pass
    return records


def print_table(records):
    if not records:
        print("❌ 无记录")
        return
    print("\n" + "="*120)
    print("  NSBC Phase 2 完整压测矩阵 — 汇总")
    print("="*120)
    hdr = f"{'Tag':<18}{'Mode':<8}{'CPS':>5}{'Dur':>4}{'Wrk':>4}{'Users':>7}{'Wall':>6}{'Succ%':>8}{'Total':>7}{'Timeout':>8}{'Alive':>8}{'RSS→':>10}{'Thr→':>10}{'CDR→':>10}{'RingΔ':>7}"
    print(hdr)
    print("-"*120)
    for r in records:
        tag = r.get("tag","")
        mode = r.get("mode","")
        cps = r.get("cps","")
        dur = r.get("duration","")
        wrk = r.get("workers","")
        usr = r.get("users","")
        wall = r.get("wall_time","")
        succ = r.get("success_rate","")
        tot = r.get("total_calls","")
        tmo = r.get("timeout_count","")
        alive = r.get("alive", r.get("reason",""))
        rssd = f"{r.get('vmrss_before',0)}→{r.get('vmrss_after',0)}"
        thrd = f"{r.get('threads_before',0)}→{r.get('threads_after',0)}"
        cdrd = f"{r.get('cdr_before',0)}→{r.get('cdr_after',0)}"
        try:
            ringd = int(r.get("ring_after",0)) - int(r.get("ring_before",0))
        except:
            ringd = "?"
        print(f"{tag:<18}{mode:<8}{str(cps):>5}{str(dur):>4}{str(wrk):>4}{str(usr):>7}{str(wall):>6}{str(succ):>8}{str(tot):>7}{str(tmo):>8}{str(alive):>8}{str(rssd):>10}{str(thrd):>10}{str(cdrd):>10}{str(ringd):>7}")
    print("="*120)
    bypass = [r for r in records if r.get("mode")=="BYPASS"]
    relay = [r for r in records if r.get("mode")=="RELAY"]
    print(f"\nBYPASS: {len(bypass)} 档, 存活 {sum(1 for r in bypass if r.get('alive')=='✅')}")
    print(f"RELAY:  {len(relay)} 档, 存活 {sum(1 for r in relay if r.get('alive')=='✅')}")
    if bypass:
        cps_list = [r.get("cps") for r in bypass if r.get("alive")=="✅"]
        print(f"BYPASS 存活最高 CPS: {max(cps_list) if cps_list else '无'}")
    if relay:
        cps_list = [r.get("cps") for r in relay if r.get("alive")=="✅"]
        print(f"RELAY 存活最高 CPS:  {max(cps_list) if cps_list else '无'}")


def main():
    local_out = LOCAL_OUT
    print(f"[连接] {REMOTE_HOST}:{REMOTE_PORT}")
    print(f"[输出] {local_out}")
    t0 = time.time()

    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(REMOTE_HOST, port=REMOTE_PORT, username=REMOTE_USER, password=REMOTE_PASS, timeout=30)
    print(f"[SSH] ✅ 连接 (耗时 {int(time.time()-t0)}s)")

    try:
        run_remote(ssh, make_bash(), timeout=2400)
        collect(ssh, local_out)
        records = parse_summary(local_out)
        print_table(records)

        # 保存 summary.json
        with open(os.path.join(local_out, "summary.json"), "w", encoding="utf-8") as f:
            json.dump(records, f, ensure_ascii=False, indent=2)
        print(f"\n📊 汇总 → {os.path.join(local_out, 'summary.json')}")

    except Exception as e:
        print(f"\n❌ 失败: {e}")
        import traceback; traceback.print_exc()
    finally:
        ssh.close()
        print(f"\n[SSH] 关闭. 总耗时: {int(time.time()-t0)}s")


if __name__ == "__main__":
    main()
