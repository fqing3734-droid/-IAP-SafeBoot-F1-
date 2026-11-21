# 一键调用 Keil uVision5 (UV4.exe) 编译 SafeBoot-F1 全部 4 个固件镜像：
#   1) SafeBoot_Boot.bin / .hex  (0x08000000, <= 8KB)
#   2) SafeBoot_App_v10.bin      (0x08002000, v1.0 正常固件)
#   3) SafeBoot_App_v11.bin      (0x08002000, v1.1 升级目标固件)
#   4) SafeBoot_App_v12_bad.bin  (0x08002000, v1.2-BAD 死锁故障注入固件)

$ErrorActionPreference = 'Stop'

$mdkDir = $PSScriptRoot
$uv4    = 'E:\keil5\UV4\UV4.exe'
if (-not (Test-Path -LiteralPath $uv4)) {
    throw "找不到 Keil UV4.exe: $uv4"
}

function Invoke-KeilBuild {
    param([string]$Proj, [string]$Target, [string]$LogName)
    $projPath = Join-Path $mdkDir $Proj
    $logPath  = Join-Path $mdkDir $LogName
    if (Test-Path -LiteralPath $logPath) { Remove-Item -LiteralPath $logPath -Force }

    Write-Host "--- Building $Proj [$Target] ..." -ForegroundColor Cyan
    $p = Start-Process -FilePath $uv4 -ArgumentList @('-r', $projPath, '-t', $Target, '-o', $logPath, '-j0') -Wait -PassThru
    $log = if (Test-Path -LiteralPath $logPath) { Get-Content -LiteralPath $logPath -Raw } else { '(no log)' }
    Write-Host $log

    # UV4 退出码：0=无错无警告，1=有警告无错，>1=编译失败
    if ($p.ExitCode -gt 1) {
        throw "Keil build failed for $Proj [$Target] (exit=$($p.ExitCode))"
    }
}

Invoke-KeilBuild -Proj 'SafeBoot_Boot.uvprojx' -Target 'SafeBoot_Boot' -LogName 'build_boot.log'
Invoke-KeilBuild -Proj 'SafeBoot_App.uvprojx'  -Target 'App_v10'       -LogName 'build_app_v10.log'
Invoke-KeilBuild -Proj 'SafeBoot_App.uvprojx'  -Target 'App_v11'       -LogName 'build_app_v11.log'
Invoke-KeilBuild -Proj 'SafeBoot_App.uvprojx'  -Target 'App_v12_Bad'   -LogName 'build_app_v12_bad.log'

Write-Host "`n=== 产物清单 (MDK\Objects\) ===" -ForegroundColor Green
Get-ChildItem (Join-Path $mdkDir 'Objects\*.bin'), (Join-Path $mdkDir 'Objects\*.hex') -ErrorAction SilentlyContinue |
    Select-Object Name, Length | Format-Table -AutoSize
