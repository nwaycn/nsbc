#!/usr/bin/env bash
# setup-acme-cache.sh — 在生产服务器上创建 certmagic 证书缓存目录并写入 website.ini
#
# 用法: sudo ./setup-acme-cache.sh [website.ini 路径]
#   默认会在当前目录找 website.ini；也可以显式传路径
#
# 完成后 website.ini 的 [acme] 段会被修改为：
#   enable=true
#   domain=nsbc.nway.com.cn
#   email=1354608370@qq.com
#   cache_dir=/var/lib/nsbc/website/acme-cache

set -euo pipefail

CACHE_DIR="/var/lib/nsbc/website/acme-cache"
INI_PATH="${1:-website.ini}"
DOMAIN="nsbc.nway.com.cn"
EMAIL="1354608370@qq.com"

# ---- 1. 创建证书缓存目录 ----
if [ "$(id -u)" -ne 0 ]; then
	echo "⚠️  目录 /var/lib/nsbc/website 需要 root 权限，正在用 sudo 创建..."
	sudo mkdir -p "$CACHE_DIR"
	sudo chown -R "${SUDO_USER:-$USER}:${SUDO_USER:-$USER}" /var/lib/nsbc
else
	mkdir -p "$CACHE_DIR"
fi
echo "✅ 证书缓存目录已创建: $CACHE_DIR"

# ---- 2. 备份并改写 website.ini 的 [acme] 段 ----
if [ ! -f "$INI_PATH" ]; then
	echo "❌ 找不到 $INI_PATH，请把 website.ini 路径作为脚本参数传入"
	exit 1
fi

cp -a "$INI_PATH" "${INI_PATH}.bak.$(date +%Y%m%d%H%M%S)"

# 核心思路：INI 里 [acme] 段可能不存在、或 enable/cache_dir 行存在/不存在，
# 用 awk 原地替换 [acme] 段的 4 个关键键，其余字段原样保留。
awk -v domain="$DOMAIN" -v email="$EMAIL" -v cdir="$CACHE_DIR" '
BEGIN { in_acme = 0; wrote_section = 0 }
/^\s*\[acme\]/ {
	in_acme = 1
	wrote_section = 1
	print $0
	# 强制覆盖的 4 行（后面遇到会跳过）
	printf "enable  = true\n"
	printf "domain  = %s\n", domain
	printf "email   = %s\n", email
	printf "cache_dir = %s\n", cdir
	printf "staging = false\n"
	next
}
in_acme {
	if (/^\s*\[/) {
		# 下一个 section 开始，先补一个空行再输出当前行
		in_acme = 0
		print ""
		print $0
		next
	}
	# 覆盖/跳过 [acme] 段里这些键 —— 上面已经用固定值打印过
	if (   $0 ~ /^\s*enable\s*=/
	   || $0 ~ /^\s*domain\s*=/
	   || $0 ~ /^\s*email\s*=/
	   || $0 ~ /^\s*cache_dir\s*=/
	   || $0 ~ /^\s*staging\s*=/) {
		next
	}
	print $0
	next
}
{
	# 所有非 [acme] 段落原样输出
	print $0
}
END {
	if (!wrote_section) {
		# 整个文件没有 [acme] 段，追加一个
		print ""
		print "[acme]"
		printf "enable  = true\n"
		printf "domain  = %s\n", domain
		printf "email   = %s\n", email
		printf "cache_dir = %s\n", cdir
		printf "staging = false\n"
	}
}
' "$INI_PATH" > "${INI_PATH}.tmp" && mv "${INI_PATH}.tmp" "$INI_PATH"

echo "✅ $INI_PATH 的 [acme] 段已更新:"
echo "   enable    = true"
echo "   domain    = $DOMAIN"
echo "   email     = $EMAIL"
echo "   cache_dir = $CACHE_DIR"
echo "   staging   = false"
echo ""
echo "下一步: sudo -u <网站运行用户> ./nsbc-site.exe"
echo "   certmagic 会自动在 :80 完成 HTTP-01 挑战、:443 签发证书并后台续签"
echo "   签发首次成功约 30–60 秒，期间 :80 不可被其他程序占用"
