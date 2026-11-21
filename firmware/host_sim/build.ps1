# 构建 SafeBoot-F1 PC 端 SIL 仿真器（safeboot_sim.exe）
# 规避路径含中文导致 MinGW gcc 无法打开文件的问题：复制到纯 ASCII 临时目录编译，再将产物拷回 host_sim\。

$ErrorActionPreference = 'Stop'

$simDir = $PSScriptRoot
$fw     = Split-Path -Parent $simDir
$tmp    = Join-Path $env:TEMP 'safeboot_host_sim_build'
$gcc    = 'D:\Cyuyan\mingw64\bin\gcc.exe'

if (-not (Test-Path -LiteralPath $gcc)) {
    $gcc = (Get-Command gcc -ErrorAction SilentlyContinue).Source
}
if (-not $gcc) { throw '找不到 gcc，请确认 MinGW64 已安装' }
$env:PATH = "$(Split-Path -Parent $gcc);$env:PATH"

if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
New-Item -ItemType Directory -Path $tmp | Out-Null

$files = @(
    'common\common.h',
    'common\crc16.h',
    'common\crc16.c',
    'common\crc32.h',
    'common\crc32.c',
    'bsp\bsp_flash.h',
    'middleware\boot_meta.h',
    'middleware\boot_meta.c',
    'middleware\iap_proto.h',
    'middleware\iap_proto.c',
    'host_sim\safeboot_sim.c'
)

foreach ($f in $files) {
    Copy-Item -LiteralPath (Join-Path $fw $f) -Destination $tmp
}

$outTmp = Join-Path $tmp 'safeboot_sim.exe'
& $gcc -std=c99 -O2 -Wall -Wextra -Werror -DHOST_SIM -o $outTmp `
    (Join-Path $tmp 'safeboot_sim.c') `
    (Join-Path $tmp 'boot_meta.c') `
    (Join-Path $tmp 'iap_proto.c') `
    (Join-Path $tmp 'crc16.c') `
    (Join-Path $tmp 'crc32.c')
if ($LASTEXITCODE -ne 0) { throw 'safeboot_sim.exe 编译失败' }

$dstExe = Join-Path $simDir 'safeboot_sim.exe'
Copy-Item -LiteralPath $outTmp -Destination $dstExe -Force
Write-Host "OK -> $dstExe" -ForegroundColor Green
