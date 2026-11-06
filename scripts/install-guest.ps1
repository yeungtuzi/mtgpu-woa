<#
安装 Windows guest 侧的全部配置。管理员 PowerShell 执行：

    powershell -ExecutionPolicy Bypass -File install-guest.ps1 [-WithAgent]

这里只做 RDP 本身的配置。分辨率、DPI、剪贴板、音频都是 RDP 协议通道
（DisplayControl / cliprdr / rdpsnd）负责的，guest 里不需要自定义服务。

  - 开 RDP 并放行防火墙
  - 解除 RDP 的 30fps 节流
  - 打开 H.264/AVC 硬件编码
  - 允许同一用户多会话（否则 /admin 之外的连接会被踢）
  - 关掉锁屏/睡眠/休眠，虚拟机没必要省电

-WithAgent 才会额外部署可选的会话助手（额外的状态查询/调优用途）。
#>
param([switch]$WithAgent)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$agentSrc = Join-Path $here 'winvm-agent.ps1'

function Info($m) { Write-Host "  $m" }
function Step($m) { Write-Host "== $m" }

# ---------------------------------------------------------------- 0. 前置检查
Step '前置检查'
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw '需要以管理员身份运行' }
Info ("Windows " + [Environment]::OSVersion.Version + " / " + $env:PROCESSOR_ARCHITECTURE)

# ---------------------------------------------------------------- 1. 会话助手
Step '部署会话助手（可选）'
if ($WithAgent) {
if (-not (Test-Path $agentSrc)) { throw "找不到 $agentSrc" }
Copy-Item $agentSrc 'C:\winvm-agent.ps1' -Force
Info 'C:\winvm-agent.ps1'

# 助手只做 RDP 之外的补充（状态查询、电源调优）。
# 分辨率 / DPI / 剪贴板 / 音频 全部由 RDP 协议通道负责，不需要助手参与。
$action = New-ScheduledTaskAction -Execute 'powershell.exe' `
  -Argument '-WindowStyle Hidden -ExecutionPolicy Bypass -File C:\winvm-agent.ps1'
$trigger = New-ScheduledTaskTrigger -AtLogOn
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
  -StartWhenAvailable -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName 'WinvmAgent' -Action $action -Trigger $trigger `
  -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName 'WinvmAgent'
Start-Sleep -Seconds 2
Info ("计划任务 WinvmAgent = " + (Get-ScheduledTask -TaskName 'WinvmAgent').State)
} else {
  Info '跳过（加 -WithAgent 才会安装）'
}

# ---------------------------------------------------------------- 1b. qemu-ga
# 软关机/软重启走 qemu-ga（guest 内正常关机，程序能存盘）。
# ARM64 没有官方 MSI，所以随附一包上游原样二进制（见 guest/qemu-ga/NOTICE.txt）。
Step '安装 qemu-ga（可选但推荐）'
$qgaSrc = Join-Path $here 'qemu-ga'
if (Test-Path (Join-Path $qgaSrc 'qemu-ga.exe')) {
  $dst = 'C:\Program Files\qemu-ga'
  New-Item -ItemType Directory -Force $dst | Out-Null
  Copy-Item (Join-Path $qgaSrc '*') $dst -Force -Recurse
  & (Join-Path $dst 'qemu-ga.exe') -s install | Out-Null
  Start-Sleep -Seconds 2
  if (Test-Path 'HKLM:\SYSTEM\CurrentControlSet\Services\qemu-ga') {
    Start-Service qemu-ga -ErrorAction SilentlyContinue
    Info ("服务已注册，状态 " + (Get-Service qemu-ga).Status)
  } else {
    Info '服务注册失败（可能未以管理员运行）'
  }
} else {
  Info '未找到 qemu-ga 目录，跳过；winvm stop 会退回 ACPI 电源键'
}

# ---------------------------------------------------------------- 2. RDP
Step '启用远程桌面'
Set-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server' -Name fDenyTSConnections -Value 0 -Type DWord
Set-Service TermService -StartupType Automatic
Start-Service TermService
# 规则名随语言变化，直接用稳定标识
foreach ($r in 'RemoteDesktop-UserMode-In-TCP', 'RemoteDesktop-UserMode-In-UDP', 'RemoteDesktop-Shadow-In-TCP') {
  Enable-NetFirewallRule -Name $r -ErrorAction SilentlyContinue | Out-Null
}
Info 'TermService 已启动，防火墙规则已放行'

# ---------------------------------------------------------------- 3. 解除 30fps
Step '解除 RDP 的 30fps 节流'
# 微软文档：远程会话默认不支持 >30fps，DWMFRAMEINTERVAL 以毫秒计。
# 15ms ≈ 66fps。两个键都写，写完必须读回确认。
$ws = 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations'
Set-ItemProperty $ws -Name DWMFRAMEINTERVAL -Value 15 -Type DWord
Set-ItemProperty "$ws\RDP-Tcp" -Name DWMFRAMEINTERVAL -Value 15 -Type DWord
$back = (Get-ItemProperty $ws -Name DWMFRAMEINTERVAL).DWMFRAMEINTERVAL
if ($back -ne 15) { throw "DWMFRAMEINTERVAL 写入失败（读回 $back）" }
Info "DWMFRAMEINTERVAL = $back（读回确认）"

# ---------------------------------------------------------------- 4. 硬件编码
Step '启用 H.264/AVC 硬件编码'
$ts = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services'
New-Item -Path $ts -Force | Out-Null
Set-ItemProperty $ts -Name AVCHardwareEncodePreferred -Value 1 -Type DWord
Set-ItemProperty $ts -Name AVC444ModePreferred       -Value 1 -Type DWord
Info 'AVCHardwareEncodePreferred=1, AVC444ModePreferred=1'

# ---------------------------------------------------------------- 5. 多会话
Step '允许同一用户多会话'
# 否则第二个连接会在 ~11 秒后被 ERRINFO_RPC_INITIATED_DISCONNECT 踢掉
Set-ItemProperty $ts -Name fSingleSessionPerUser -Value 0 -Type DWord
$ss = (Get-ItemProperty $ts).fSingleSessionPerUser
if ($ss -ne 0) { throw "fSingleSessionPerUser 写入失败（读回 $ss）" }
Info "fSingleSessionPerUser = $ss"

# ---------------------------------------------------------------- 6. 省电
Step '关闭锁屏/睡眠/休眠'
powercfg /change monitor-timeout-ac 0
powercfg /change standby-timeout-ac 0
powercfg /change hibernate-timeout-ac 0
powercfg /hibernate off
Info '已关闭'

# ---------------------------------------------------------------- 完成
Step '完成'
Write-Host @'
需要重启一次让 DWMFRAMEINTERVAL 与硬件编码策略生效。

重启后自检：
  Get-Service WinvmAgent                    # 应为 Running
  query session                             # 应看到 console 或 rdp-tcp 会话
  Get-Process explorer                      # 有桌面
'@
