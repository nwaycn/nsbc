# NSBC 三端互呼测试 — Windows 快速启动
# 一次性装好依赖后，以后直接 `./run.ps1` 即可。
#
# 前置条件：
#   1) Python 3.10+ （https://www.python.org/downloads/），安装时勾选 "Add to PATH"
#   2) NSBC 运行机可达（WSS :5062），账号已在 DB 中 seed
#   3) 自签 TLS 证书已在 NSBC 侧 OK（本脚本 allow_self_signed=true）
#
# 用法：
#   ./run.ps1                              # 跑全 6 对
#   ./run.ps1 -Config config.json          # 用自定义配置
#   ./run.ps1 -Single android,desktop      # 只跑 Android↔Desktop 两方向
#   ./run.ps1 -Single desktop->ios         # 只跑 Desktop→iOS 单向
#   ./run.ps1 -Server 192.168.1.100         # 临时换服务器

param(
    [string]$Config  = "",
    [string]$Server  = "",
    [string]$Single  = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Here

# ---- 1. 找 Python ----
$python = $null
foreach ($candidate in @("python", "py -3", "python3")) {
    try {
        $v = & Invoke-Expression "$candidate --version 2>&1"
        if ($LASTEXITCODE -eq 0) { $python = $candidate; break }
    } catch {}
}
if (-not $python) {
    Write-Error @"
❌ 未找到 Python。请先安装 Python 3.10+：
   https://www.python.org/downloads/
   安装时务必勾选 "Add Python to PATH"。
"@
}
Write-Host "[run] Python OK: $python"

# ---- 2. 依赖检查 ----
& Invoke-Expression "$python -c 'import websockets'" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "[run] 首次运行，安装 websockets ..."
    & Invoke-Expression "$python -m pip install --quiet websockets"
    if ($LASTEXITCODE -ne 0) { Write-Error "pip install websockets 失败" }
}

# ---- 3. 组装参数 ----
$args_list = @("test_mutual_calls.py")
if ($Config)    { $args_list += "--config";  $args_list += $Config }
if ($Server)    { $args_list += "--server";  $args_list += $Server }
if ($Single)    { $args_list += "--single";  $args_list += $Single }

Write-Host "[run] python $($args_list -join ' ')"
Write-Host ""
& Invoke-Expression "$python $($args_list -join ' ')"
$code = $LASTEXITCODE

Write-Host ""
if ($code -eq 0) {
    Write-Host "✅ 全部测试通过"
} else {
    Write-Host "❌ 有失败（退出码 $code）"
}
exit $code
