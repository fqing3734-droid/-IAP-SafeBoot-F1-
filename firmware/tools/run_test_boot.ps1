# 在 PC 上编译并运行 SafeBoot-F1 核心单元测试（test_boot.c）
# 规避路径含中文导致 MinGW gcc 无法打开文件的问题：复制必要源码至纯 ASCII 临时目录编译运行。

$ErrorActionPreference = 'Stop'

$fw  = Split-Path -Parent $PSScriptRoot
$tmp = Join-Path $env:TEMP 'safeboot_unit_test'
$gcc = 'D:\Cyuyan\mingw64\bin\gcc.exe'

if (-not (Test-Path -LiteralPath $gcc)) {
    $gcc = (Get-Command gcc -ErrorAction SilentlyContinue).Source
}
if (-not $gcc) { throw '找不到 gcc，请确认 MinGW64 已安装' }
$gccDir = Split-Path -Parent $gcc
$env:PATH = "$gccDir;$env:PATH"

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
    'tools\test_boot.c'
)

foreach ($f in $files) {
    Copy-Item -LiteralPath (Join-Path $fw $f) -Destination $tmp
}

$exe = Join-Path $tmp 'test_boot.exe'
& $gcc -std=c99 -O2 -Wall -Wextra -Werror -o $exe `
    (Join-Path $tmp 'test_boot.c') `
    (Join-Path $tmp 'boot_meta.c') `
    (Join-Path $tmp 'iap_proto.c') `
    (Join-Path $tmp 'crc16.c') `
    (Join-Path $tmp 'crc32.c')
if ($LASTEXITCODE -ne 0) { throw 'gcc 编译失败' }

& $exe
$code = $LASTEXITCODE
Write-Host ''
if ($code -eq 0) {
    Write-Host '==> SafeBoot-F1 PC 单元测试全部通过' -ForegroundColor Green
} else {
    Write-Host '==> 存在失败用例，请检查上方 FAIL 输出' -ForegroundColor Red
}
exit $code
