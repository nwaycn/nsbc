#!/usr/bin/env bash
# =============================================================================
# NSBC GeoIP CN 白名单一键更新脚本
#
# 功能：
#   1. 从 apnic.cn 拉取最新 CN IPv4 分配列表
#   2. 转换成 CIDR 格式，去重排序
#   3. 推送到 nsbcd 应用层 allowlist（热重载，无需重启）
#
# 使用：
#   ./scripts/geoip-update.sh                # 手动运行
#   ./scripts/geoip-update.sh --dry-run     # 只拉取转换，不推送
#   ./scripts/geoip-update.sh --restart      # 推送失败时 fallback systemctl restart
#
# 依赖：apt install -y curl awk coreutils netcat-openbsd
# =============================================================================
set -uo pipefail   # 故意不加 -e：中间任何一步失败都要继续跑完，最后统一报 err；
                   # 否则管道里 grep 没匹配就触发 -e 静默退出，用户看不到日志

# ---- 配置 ----
# 优先国内 CDN + 条数量匹配（~7460 条 ≤ BL_CIDR_MAX 16384）
CN_CIDR_SOURCES=(
    # 1. jsDelivr CDN 镜像 ACL4SSR —— 国内最快（上次阿里云实测 260KB 秒下）
    "https://cdn.jsdelivr.net/gh/ACL4SSR/ACL4SSR@master/Clash/ChinaIp.list"
    # 2. GitHub 直连 ACL4SSR（同样 7460 条 Clash IP-CIDR 格式）
    "https://raw.githubusercontent.com/ACL4SSR/ACL4SSR/refs/heads/master/Clash/ChinaIp.list"
    # 3. zhufengme/block_cn_files —— 纯 CIDR 格式，但 30488 条超过 BL_CIDR_MAX，会被截断到 16384 条
    "https://raw.githubusercontent.com/zhufengme/block_cn_files/master/cn_ip_list.txt"
    # 4. APNIC raw 兜底（4MB 慢，需要 awk 转换，国内服务器常超时）
    "https://ftp.apnic.net/apnic/stats/apnic/delegated-apnic-latest"
)
CACHE_DIR="/var/cache/nsbc-geoip"
OUTPUT_FILE="${CACHE_DIR}/cn_cidrs.txt"
UDS_SOCK="/var/run/nsbc/control.sock"

# NSBC_HOME 自动探测：优先级 环境变量 > 同级 conf/nsbc.yaml > PATH 里的 nsbcd > /proc > /opt/nsbc
if [ -z "${NSBC_HOME:-}" ]; then
    # 方法 1：当前工作目录或其父目录有 conf/nsbc.yaml（脚本在 deploy-root/scripts/ 执行）
    for try in "$(pwd)" "$(dirname "$(pwd)")" "$(dirname "$(dirname "$(pwd)")")"; do
        if [ -f "${try}/conf/nsbc.yaml" ]; then
            NSBC_HOME="$try"; break
        fi
    done
    # 方法 2：PATH 里 nsbcd 可执行文件同级
    if [ -z "${NSBC_HOME:-}" ] && command -v nsbcd &>/dev/null; then
        EXE_DIR=$(dirname "$(readlink -f "$(command -v nsbcd)")")
        if [ -f "${EXE_DIR}/conf/nsbc.yaml" ]; then
            NSBC_HOME="$EXE_DIR"
        elif [ -f "${EXE_DIR}/../conf/nsbc.yaml" ]; then
            NSBC_HOME=$(dirname "$EXE_DIR")
        fi
    fi
    # 方法 3：fallback
    if [ -z "${NSBC_HOME:-}" ]; then
        NSBC_HOME="/opt/nsbc"
    fi
fi
GEO_TARGET="${NSBC_HOME}/geo/cn_cidrs.txt"
CURL_MAX_TIME=120
CURL_RETRIES=3

DRY_RUN=0
FALLBACK_RESTART=0
for arg in "$@"; do
    case "$arg" in
        --dry-run)    DRY_RUN=1 ;;
        --restart)    FALLBACK_RESTART=1 ;;
        -h|--help)
            sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
    esac
done

log()  { echo "[$(date '+%F %T')] $*"; }
warn() { echo "[$(date '+%F %T')] ⚠️  $*" >&2; }
err()  { echo "[$(date '+%F %T')] ❌ $*" >&2; exit 1; }

# ---- 1. 依赖检查 ----
for cmd in curl awk sort nc; do
    command -v "$cmd" &>/dev/null || err "缺少依赖: $cmd（apt install -y curl awk coreutils netcat-openbsd）"
done

# ---- 2. 多源降级下载 ----
mkdir -p "$CACHE_DIR"
RAW_FILE="${CACHE_DIR}/apnic_latest.raw"    # APNIC raw 才会写这个
DL_FILE="${CACHE_DIR}/downloaded.txt"        # 所有源统一下载到这里

download_source() {
    local url="$1"
    local label="$2"
    log "尝试 [${label}] ${url}"

    if curl -fsSL --connect-timeout 10 --max-time "$CURL_MAX_TIME" \
             --retry "$CURL_RETRIES" --retry-delay 2 \
             -o "$DL_FILE" "$url"; then
        local lines size
        lines=$(wc -l < "$DL_FILE" 2>/dev/null || echo 0)
        size=$(stat -c%s "$DL_FILE" 2>/dev/null || echo 0)
        if [ "$size" -lt 1000 ] || [ "$lines" -lt 50 ]; then
            warn "${label} 文件太小（${size}B, ${lines}行），跳过"
            return 1
        fi
        log "✅ ${label} 下载成功：${size}B / ${lines} 行"
        return 0
    fi
    warn "${label} 下载失败"
    return 1
}

DOWNLOAD_OK=0
for i in "${!CN_CIDR_SOURCES[@]}"; do
    url="${CN_CIDR_SOURCES[$i]}"
    label="源$((i+1))"
    if download_source "$url" "$label"; then
        DOWNLOAD_OK=1
        break
    fi
    sleep 1
done

[ "$DOWNLOAD_OK" -eq 1 ] || err "所有下载源均失败 —— 检查服务器外网：curl -I https://www.baidu.com"

# ---- 3. 格式识别 → 统一转 CIDR ----
# 三种格式：
#   A) 纯 CIDR：1.0.1.0/24         → 直接复制
#   B) Clash：  IP-CIDR,1.0.1.0/24 → grep + sed 剥前缀
#   C) APNIC：  apnic|CN|ipv4|ip|count|... → awk 转换

FMT_PURE=0 FMT_CLASH=0 FMT_APNIC=0

# 跳过注释/空行，找第一个有效行来判断格式
FIRST_LINE=$(grep -vE '^\s*#|^\s*$' "$DL_FILE" | head -1)
echo "$FIRST_LINE" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+/[0-9]+$' && FMT_PURE=1
echo "$FIRST_LINE" | grep -qE '^IP-CIDR,[0-9]+\.' && FMT_CLASH=1
echo "$FIRST_LINE" | grep -qE '^apnic\|CN\|ipv4\|' && FMT_APNIC=1

log "格式探测（首个有效行: ${FIRST_LINE:0:60}...）pure=${FMT_PURE} clash=${FMT_CLASH} apnic=${FMT_APNIC}"

if [ "$FMT_PURE" -eq 1 ]; then
    log "格式 A（纯 CIDR）→ 去注释去空行"
    grep -vE '^\s*#|^\s*$' "$DL_FILE" | sort -u > "${OUTPUT_FILE}.tmp"
elif [ "$FMT_CLASH" -eq 1 ]; then
    log "格式 B（Clash IP-CIDR）→ grep + 剥前缀"
    grep -oE 'IP-CIDR,[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+/[0-9]+' "$DL_FILE" \
      | sed 's/^IP-CIDR,//' | sort -u > "${OUTPUT_FILE}.tmp"
elif [ "$FMT_APNIC" -eq 1 ]; then
    log "格式 C（APNIC raw）→ awk 转换"
    cp "$DL_FILE" "$RAW_FILE"
    awk -F'|' '$2=="CN" && $3=="ipv4" {
        ip=$4; count=$5
        if (count >= 4294967296) p=0
        else if (count >= 2147483648) p=1
        else if (count >= 1073741824) p=2
        else if (count >= 536870912) p=3
        else if (count >= 268435456) p=4
        else if (count >= 134217728) p=5
        else if (count >= 67108864) p=6
        else if (count >= 33554432) p=7
        else if (count >= 16777216) p=8
        else if (count >= 8388608) p=9
        else if (count >= 4194304) p=10
        else if (count >= 2097152) p=11
        else if (count >= 1048576) p=12
        else if (count >= 524288) p=13
        else if (count >= 262144) p=14
        else if (count >= 131072) p=15
        else if (count >= 65536) p=16
        else if (count >= 32768) p=17
        else if (count >= 16384) p=18
        else if (count >= 8192) p=19
        else if (count >= 4096) p=20
        else if (count >= 2048) p=21
        else if (count >= 1024) p=22
        else if (count >= 512) p=23
        else if (count >= 256) p=24
        else if (count >= 128) p=25
        else if (count >= 64) p=26
        else if (count >= 32) p=27
        else if (count >= 16) p=28
        else if (count >= 8) p=29
        else if (count >= 4) p=30
        else if (count >= 2) p=31
        else p=32
        printf "%s/%d\n", ip, p
    }' "$RAW_FILE" | sort -u > "${OUTPUT_FILE}.tmp"
else
    err "无法识别文件格式 — 前 3 行：$(head -3 "$DL_FILE" | tr '\n' '|')"
fi

# 去掉 # 注释和空行（纯 CIDR / Clash 文件可能带）
grep -vE '^\s*#|^\s*$' "${OUTPUT_FILE}.tmp" | sort -u > "${OUTPUT_FILE}.clean"
mv "${OUTPUT_FILE}.clean" "${OUTPUT_FILE}.tmp"

CN_COUNT=$(wc -l < "${OUTPUT_FILE}.tmp")
if [ "$CN_COUNT" -lt 1000 ]; then
    err "CN CIDR 数量异常（${CN_COUNT} 条），可能数据源变化。中止以防清空 allowlist。"
fi
BL_CIDR_MAX=16384   # C 代码里的上限；超过会 overflow 被截断
if [ "$CN_COUNT" -gt "$BL_CIDR_MAX" ]; then
    warn "CN CIDR ${CN_COUNT} 条超过 BL_CIDR_MAX=${BL_CIDR_MAX}，nsbcd 加载时会截断到前 ${BL_CIDR_MAX} 条"
    warn "建议改 security.c 里的 BL_CIDR_MAX，或换源（ACL4SSR 仅 ~7460 条）"
fi

# ---- 4. 原子替换 ----
mv "${OUTPUT_FILE}.tmp" "$OUTPUT_FILE"
log "✅ 生成 ${CN_COUNT} 条 CN IPv4 CIDR → ${OUTPUT_FILE}"

# ---- 5. 复制到 nsbcd 默认种子路径 ----
# security_init 会检测 $NSBC_HOME/geo/cn_cidrs.txt，nsbc.yaml 没配 allow_cidr_file 时自动加载
mkdir -p "$(dirname "$GEO_TARGET")"
cp -f "$OUTPUT_FILE" "$GEO_TARGET"
log "✅ 种子已同步到 ${GEO_TARGET}"

[ "$DRY_RUN" -eq 1 ] && { log "--dry-run：到此结束"; exit 0; }

# ---- 6. 推送到 nsbcd（热重载 control.sock）----
if [ -S "$UDS_SOCK" ]; then
    log "通过 UDS 热重载 security.geoip.reload ..."
    RESP=$(echo "security.geoip.reload" | nc -w 5 -U "$UDS_SOCK" 2>/dev/null || true)
    if echo "$RESP" | grep -q "^OK"; then
        log "✅ nsbcd 热重载成功 — allowlist 模式立即生效，零停机"
        # 顺便打印当前状态
        echo "security.geoip.list" | nc -w 3 -U "$UDS_SOCK" 2>/dev/null || true
    else
        warn "UDS 热重载失败（响应: ${RESP:-empty}）"
        if [ "$FALLBACK_RESTART" -eq 1 ]; then
            log "尝试 systemctl restart nsbcd ..."
            if command -v systemctl &>/dev/null; then
                systemctl restart nsbcd || warn "systemctl restart nsbcd 失败"
                systemctl restart nsbc-web || warn "systemctl restart nsbc-web 失败"
                log "✅ 已重启 systemd 服务"
            else
                warn "无 systemctl，手动重启 nsbcd 进程"
            fi
        else
            warn "加 --restart 参数可自动重启服务"
        fi
    fi
else
    warn "UDS socket 不存在：${UDS_SOCK}"
    warn "nsbcd 可能未运行 — geoip 文件已就位，下次启动自动加载"
fi

log "完成。"
