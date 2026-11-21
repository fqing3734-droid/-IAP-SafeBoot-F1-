# 生成 SafeBoot_Boot.uvprojx/.uvoptx 与 SafeBoot_App.uvprojx/.uvoptx
# 基于本机已验证可编译且配置好 ST-Link V2 的 RIO.uvprojx / RIO.uvoptx 模板，
# 全程使用字面 .Replace()，绝不使用正则 -replace（防止 $$Device 被吞掉）。

$ErrorActionPreference = 'Stop'

$mdkDir  = $PSScriptRoot
$tplProj = 'D:\Relocated\DeskBox\文件夹\rio_module\firmware\MDK\RIO.uvprojx'
$tplOpt  = 'D:\Relocated\DeskBox\文件夹\rio_module\firmware\MDK\RIO.uvoptx'

if (-not (Test-Path -LiteralPath $tplProj)) { throw "模板不存在: $tplProj" }
if (-not (Test-Path -LiteralPath $tplOpt))  { throw "模板不存在: $tplOpt" }

$projTpl = [System.IO.File]::ReadAllText($tplProj, [System.Text.Encoding]::UTF8)
$optTpl  = [System.IO.File]::ReadAllText($tplOpt,  [System.Text.Encoding]::UTF8)
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Build-GroupsXml {
    param([array]$Groups)
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine('<Groups>')
    foreach ($g in $Groups) {
        [void]$sb.AppendLine('        <Group>')
        [void]$sb.AppendLine("          <GroupName>$($g.Name)</GroupName>")
        [void]$sb.AppendLine('          <Files>')
        foreach ($f in $g.Files) {
            [void]$sb.AppendLine('            <File>')
            [void]$sb.AppendLine("              <FileName>$($f.N)</FileName>")
            [void]$sb.AppendLine("              <FileType>$($f.T)</FileType>")
            [void]$sb.AppendLine("              <FilePath>$($f.P)</FilePath>")
            [void]$sb.AppendLine('            </File>')
        }
        [void]$sb.AppendLine('          </Files>')
        [void]$sb.AppendLine('        </Group>')
    }
    [void]$sb.Append('      </Groups>')
    return $sb.ToString()
}

function Build-TargetXml {
    param(
        [string]$BaseTargetXml,
        [string]$TargetName,
        [string]$OutputName,
        [string]$RomStart,
        [string]$RomSize,
        [string]$TextAddr,
        [string]$Define,
        [string]$IncludePath,
        [string]$OptimLevel,
        [string]$GroupsXml
    )
    $t = $BaseTargetXml
    $t = $t.Replace('<TargetName>RIO_4AI2DO</TargetName>', "<TargetName>$TargetName</TargetName>")
    $t = $t.Replace('<OutputName>RIO</OutputName>', "<OutputName>$OutputName</OutputName>")
    $t = $t.Replace(
        "<OCR_RVCT4>`r`n                <Type>1</Type>`r`n                <StartAddress>0x8000000</StartAddress>`r`n                <Size>0x10000</Size>`r`n              </OCR_RVCT4>",
        "<OCR_RVCT4>`r`n                <Type>1</Type>`r`n                <StartAddress>$RomStart</StartAddress>`r`n                <Size>$RomSize</Size>`r`n              </OCR_RVCT4>"
    )
    # 兼容 LF 行尾模板
    $t = $t.Replace(
        "<OCR_RVCT4>`n                <Type>1</Type>`n                <StartAddress>0x8000000</StartAddress>`n                <Size>0x10000</Size>`n              </OCR_RVCT4>",
        "<OCR_RVCT4>`n                <Type>1</Type>`n                <StartAddress>$RomStart</StartAddress>`n                <Size>$RomSize</Size>`n              </OCR_RVCT4>"
    )
    $t = $t.Replace('<TextAddressRange>0x08000000</TextAddressRange>', "<TextAddressRange>$TextAddr</TextAddressRange>")
    $t = $t.Replace('<Define>STM32F10X_MD</Define>', "<Define>$Define</Define>")
    $t = $t.Replace('<IncludePath>.\Start;..\common;..\bsp;..\middleware;..\app</IncludePath>', "<IncludePath>$IncludePath</IncludePath>")
    $t = $t.Replace('<Optim>1</Optim>', "<Optim>$OptimLevel</Optim>")

    # 启用编译后自动生成 .bin 文件，直接喂给 upgrader.py
    $afterOldCrlf = "<AfterMake>`r`n            <RunUserProg1>0</RunUserProg1>`r`n            <RunUserProg2>0</RunUserProg2>`r`n            <UserProg1Name></UserProg1Name>"
    $afterNewCrlf = "<AfterMake>`r`n            <RunUserProg1>1</RunUserProg1>`r`n            <RunUserProg2>0</RunUserProg2>`r`n            <UserProg1Name>fromelf --bin --output=.\Objects\$OutputName.bin .\Objects\$OutputName.axf</UserProg1Name>"
    $t = $t.Replace($afterOldCrlf, $afterNewCrlf)

    $afterOldLf = "<AfterMake>`n            <RunUserProg1>0</RunUserProg1>`n            <RunUserProg2>0</RunUserProg2>`n            <UserProg1Name></UserProg1Name>"
    $afterNewLf = "<AfterMake>`n            <RunUserProg1>1</RunUserProg1>`n            <RunUserProg2>0</RunUserProg2>`n            <UserProg1Name>fromelf --bin --output=.\Objects\$OutputName.bin .\Objects\$OutputName.axf</UserProg1Name>"
    $t = $t.Replace($afterOldLf, $afterNewLf)

    $gi = $t.IndexOf('<Groups>')
    $gj = $t.IndexOf('</Groups>')
    if ($gi -lt 0 -or $gj -lt 0) { throw '找不到 <Groups>' }
    $t = $t.Substring(0, $gi) + $GroupsXml + $t.Substring($gj + '</Groups>'.Length)
    return $t
}

function Write-UvOptx {
    param(
        [string]$DstPath,
        [string[]]$TargetNames,
        [array]$Groups
    )
    $lines = $optTpl -split "`r?`n"
    $startT = -1; $endT = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i].Trim() -eq '<Target>' -and $startT -lt 0) { $startT = $i }
        if ($lines[$i].Trim() -eq '</Target>' -and $endT -lt 0) { $endT = $i; break }
    }
    if ($startT -lt 0 -or $endT -lt 0) { throw 'opt 模板找不到 <Target>...</Target>' }

    $header    = ($lines[0..($startT - 1)] -join "`r`n")
    $targetTpl = ($lines[$startT..$endT] -join "`r`n")

    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine($header)

    for ($ti = 0; $ti -lt $TargetNames.Count; $ti++) {
        $tblock = $targetTpl.Replace('<TargetName>RIO_4AI2DO</TargetName>', "<TargetName>$($TargetNames[$ti])</TargetName>")
        if ($ti -gt 0) {
            $tblock = $tblock.Replace('<IsCurrentTarget>1</IsCurrentTarget>', '<IsCurrentTarget>0</IsCurrentTarget>')
        }
        [void]$sb.AppendLine($tblock)
    }

    $fileNo = 0
    for ($g = 0; $g -lt $Groups.Count; $g++) {
        $gn = $g + 1
        [void]$sb.AppendLine('')
        [void]$sb.AppendLine('  <Group>')
        [void]$sb.AppendLine("    <GroupName>$($Groups[$g].Name)</GroupName>")
        [void]$sb.AppendLine('    <tvExp>1</tvExp>')
        [void]$sb.AppendLine('    <tvExpOptDlg>0</tvExpOptDlg>')
        [void]$sb.AppendLine('    <cbSel>0</cbSel>')
        [void]$sb.AppendLine('    <RteFlg>0</RteFlg>')
        foreach ($f in $Groups[$g].Files) {
            $fileNo++
            [void]$sb.AppendLine('    <File>')
            [void]$sb.AppendLine("      <GroupNumber>$gn</GroupNumber>")
            [void]$sb.AppendLine("      <FileNumber>$fileNo</FileNumber>")
            [void]$sb.AppendLine("      <FileType>$($f.T)</FileType>")
            [void]$sb.AppendLine('      <tvExp>0</tvExp>')
            [void]$sb.AppendLine('      <tvExpOptDlg>0</tvExpOptDlg>')
            [void]$sb.AppendLine('      <bDave2>0</bDave2>')
            [void]$sb.AppendLine("      <PathWithFileName>$($f.P)</PathWithFileName>")
            [void]$sb.AppendLine("      <FilenameWithoutPath>$($f.N)</FilenameWithoutPath>")
            [void]$sb.AppendLine('      <RteFlg>0</RteFlg>')
            [void]$sb.AppendLine('      <bShared>0</bShared>')
            [void]$sb.AppendLine('    </File>')
        }
        [void]$sb.AppendLine('  </Group>')
    }
    [void]$sb.AppendLine('')
    [void]$sb.AppendLine('</ProjectOpt>')

    [System.IO.File]::WriteAllText($DstPath, $sb.ToString(), $utf8NoBom)
}

# 拆分 RIO.uvprojx 的 <Targets> 前、单 <Target> 模板、</Targets> 后
$ti0 = $projTpl.IndexOf('<Target>')
$ti1 = $projTpl.IndexOf('</Target>') + '</Target>'.Length
$projPrefix = $projTpl.Substring(0, $ti0)
$baseTarget = $projTpl.Substring($ti0, $ti1 - $ti0)
$projSuffix = $projTpl.Substring($ti1)

# ======================= 1. SafeBoot_Boot.uvprojx (0x08000000, 8KB) =======================

$bootGroups = @(
    @{ Name = 'Start'; Files = @(
        @{ N = 'startup_stm32f10x_md.s'; P = '.\Start\startup_stm32f10x_md.s'; T = 2 },
        @{ N = 'system_stm32f10x.c';     P = '.\Start\system_stm32f10x.c';     T = 1 },
        @{ N = 'core_cm3.c';             P = '.\Start\core_cm3.c';             T = 1 }
    )},
    @{ Name = 'common'; Files = @(
        @{ N = 'crc16.c';                P = '..\common\crc16.c';              T = 1 },
        @{ N = 'crc32.c';                P = '..\common\crc32.c';              T = 1 }
    )},
    @{ Name = 'bsp'; Files = @(
        @{ N = 'bsp.c';                  P = '..\bsp\bsp.c';                   T = 1 },
        @{ N = 'bsp_flash.c';            P = '..\bsp\bsp_flash.c';             T = 1 }
    )},
    @{ Name = 'middleware'; Files = @(
        @{ N = 'boot_meta.c';            P = '..\middleware\boot_meta.c';      T = 1 },
        @{ N = 'iap_proto.c';            P = '..\middleware\iap_proto.c';      T = 1 }
    )},
    @{ Name = 'boot'; Files = @(
        @{ N = 'boot_jump.c';            P = '..\boot\boot_jump.c';            T = 1 },
        @{ N = 'boot_main.c';            P = '..\boot\boot_main.c';            T = 1 }
    )}
)

$bootTarget = Build-TargetXml `
    -BaseTargetXml $baseTarget `
    -TargetName    'SafeBoot_Boot' `
    -OutputName    'SafeBoot_Boot' `
    -RomStart      '0x8000000' `
    -RomSize       '0x2000' `
    -TextAddr      '0x08000000' `
    -Define        'STM32F10X_MD' `
    -IncludePath   '.\Start;..\common;..\bsp;..\middleware;..\boot' `
    -OptimLevel    '3' `
    -GroupsXml     (Build-GroupsXml $bootGroups)

$bootProjPath = Join-Path $mdkDir 'SafeBoot_Boot.uvprojx'
$bootOptPath  = Join-Path $mdkDir 'SafeBoot_Boot.uvoptx'
[System.IO.File]::WriteAllText($bootProjPath, $projPrefix + $bootTarget + $projSuffix, $utf8NoBom)
Write-UvOptx -DstPath $bootOptPath -TargetNames @('SafeBoot_Boot') -Groups $bootGroups
Write-Host "生成: $bootProjPath (IROM1 = 0x08000000, 8KB)" -ForegroundColor Green

# ======================= 2. SafeBoot_App.uvprojx (0x08002000, 24KB, 三目标) =======================

$appGroups = @(
    @{ Name = 'Start'; Files = @(
        @{ N = 'startup_stm32f10x_md.s'; P = '.\Start\startup_stm32f10x_md.s'; T = 2 },
        @{ N = 'system_stm32f10x.c';     P = '.\Start\system_stm32f10x.c';     T = 1 },
        @{ N = 'core_cm3.c';             P = '.\Start\core_cm3.c';             T = 1 }
    )},
    @{ Name = 'common'; Files = @(
        @{ N = 'crc16.c';                P = '..\common\crc16.c';              T = 1 },
        @{ N = 'crc32.c';                P = '..\common\crc32.c';              T = 1 }
    )},
    @{ Name = 'bsp'; Files = @(
        @{ N = 'bsp.c';                  P = '..\bsp\bsp.c';                   T = 1 },
        @{ N = 'bsp_flash.c';            P = '..\bsp\bsp_flash.c';             T = 1 }
    )},
    @{ Name = 'middleware'; Files = @(
        @{ N = 'boot_meta.c';            P = '..\middleware\boot_meta.c';      T = 1 },
        @{ N = 'iap_proto.c';            P = '..\middleware\iap_proto.c';      T = 1 }
    )},
    @{ Name = 'app'; Files = @(
        @{ N = 'app_main.c';             P = '..\app\app_main.c';              T = 1 }
    )}
)

$appGroupsXml = Build-GroupsXml $appGroups

$tV10 = Build-TargetXml -BaseTargetXml $baseTarget `
    -TargetName 'App_v10' -OutputName 'SafeBoot_App_v10' `
    -RomStart '0x8002000' -RomSize '0x6000' -TextAddr '0x08002000' `
    -Define 'STM32F10X_MD,APP_VARIANT=10' `
    -IncludePath '.\Start;..\common;..\bsp;..\middleware;..\app' `
    -OptimLevel '3' -GroupsXml $appGroupsXml

$tV11 = Build-TargetXml -BaseTargetXml $baseTarget `
    -TargetName 'App_v11' -OutputName 'SafeBoot_App_v11' `
    -RomStart '0x8002000' -RomSize '0x6000' -TextAddr '0x08002000' `
    -Define 'STM32F10X_MD,APP_VARIANT=11' `
    -IncludePath '.\Start;..\common;..\bsp;..\middleware;..\app' `
    -OptimLevel '3' -GroupsXml $appGroupsXml

$tBad = Build-TargetXml -BaseTargetXml $baseTarget `
    -TargetName 'App_v12_Bad' -OutputName 'SafeBoot_App_v12_bad' `
    -RomStart '0x8002000' -RomSize '0x6000' -TextAddr '0x08002000' `
    -Define 'STM32F10X_MD,APP_VARIANT=99' `
    -IncludePath '.\Start;..\common;..\bsp;..\middleware;..\app' `
    -OptimLevel '3' -GroupsXml $appGroupsXml

$appProjPath = Join-Path $mdkDir 'SafeBoot_App.uvprojx'
$appOptPath  = Join-Path $mdkDir 'SafeBoot_App.uvoptx'
$allAppTargets = $tV10 + "`r`n    " + $tV11 + "`r`n    " + $tBad
[System.IO.File]::WriteAllText($appProjPath, $projPrefix + $allAppTargets + $projSuffix, $utf8NoBom)
Write-UvOptx -DstPath $appOptPath -TargetNames @('App_v10', 'App_v11', 'App_v12_Bad') -Groups $appGroups
Write-Host "生成: $appProjPath (IROM1 = 0x08002000, 24KB, 目标: App_v10 / App_v11 / App_v12_Bad)" -ForegroundColor Green
