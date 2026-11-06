$ErrorActionPreference = 'SilentlyContinue'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class WA {
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Ansi)]
  public struct DEVMODE {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string dmDeviceName;
    public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
    public int dmFields;
    public int dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
    public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string dmFormName;
    public short dmLogPixels;
    public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
    public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2;
    public int dmPanningWidth, dmPanningHeight;
  }
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Ansi)]
  public struct DISPLAY_DEVICE {
    public int cb;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string DeviceName;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceString;
    public int StateFlags;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceID;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceKey;
  }
  [DllImport("user32.dll")] public static extern int EnumDisplaySettings(string dev, int mode, ref DEVMODE dm);
  [DllImport("user32.dll")] public static extern int ChangeDisplaySettingsEx(string dev, ref DEVMODE dm, IntPtr hwnd, int flags, IntPtr param);
  [DllImport("user32.dll")] public static extern bool EnumDisplayDevices(string dev, int i, ref DISPLAY_DEVICE dd, int flags);
}
'@

# EnumDisplayDevices is unreliable here (the DOD adapters enumerate as nothing),
# so enumerate the screens WinForms sees and use their device names.
function Get-Displays {
  Add-Type -AssemblyName System.Windows.Forms -ErrorAction SilentlyContinue
  $out = @()
  foreach ($s in [System.Windows.Forms.Screen]::AllScreens) {
    $out += [pscustomobject]@{ name = $s.DeviceName; desc = $s.DeviceName; active = $true }
  }
  return $out
}

# the active desktop can be DISPLAY2, DISPLAY3, ... depending on which adapter
# Windows picked, so never hardcode DISPLAY1
function Get-PrimaryDisplay {
  Add-Type -AssemblyName System.Windows.Forms -ErrorAction SilentlyContinue
  try { return [System.Windows.Forms.Screen]::PrimaryScreen.DeviceName } catch {}
  return '\\.\DISPLAY1'
}

function Get-Modes([string]$dev) {
  $m = @()
  $i = 0
  while ($true) {
    $dm = New-Object WA+DEVMODE
    $dm.dmSize = [System.Runtime.InteropServices.Marshal]::SizeOf($dm)
    if ([WA]::EnumDisplaySettings($dev, $i, [ref]$dm) -eq 0) { break }
    $m += ,@($dm.dmPelsWidth, $dm.dmPelsHeight, $dm.dmDisplayFrequency)
    $i++
  }
  return $m
}

function Get-Current([string]$dev) {
  $dm = New-Object WA+DEVMODE
  $dm.dmSize = [System.Runtime.InteropServices.Marshal]::SizeOf($dm)
  [WA]::EnumDisplaySettings($dev, -1, [ref]$dm) | Out-Null
  return ,@($dm.dmPelsWidth, $dm.dmPelsHeight, $dm.dmDisplayFrequency)
}

function Set-Res([string]$dev, [int]$w, [int]$h) {
  $modes = Get-Modes $dev
  if ($modes.Count -eq 0) { return ,@(0,0,-99) }
  # 同分辨率下优先最高的刷新率：驱动默认会给很低的值（实测 1920x1080 只有 32Hz），
  # 而桌面帧率直接受它限制。
  $best = $null; $bestScore = [double]::MaxValue; $exact = $false
  foreach ($m in $modes) {
    if ($m[0] -eq $w -and $m[1] -eq $h) {
      if (-not $exact -or $m[2] -gt $best[2]) { $best = $m }
      $exact = $true
      continue
    }
    if ($exact) { continue }
    $score = [Math]::Abs($m[0] - $w) + [Math]::Abs($m[1] - $h)
    if ($score -lt $bestScore) { $bestScore = $score; $best = $m }
  }
  $dm = New-Object WA+DEVMODE
  $dm.dmSize = [System.Runtime.InteropServices.Marshal]::SizeOf($dm)
  [WA]::EnumDisplaySettings($dev, -1, [ref]$dm) | Out-Null
  $dm.dmPelsWidth = $best[0]; $dm.dmPelsHeight = $best[1]
  $dm.dmDisplayFrequency = $best[2]; $dm.dmBitsPerPel = 32
  $dm.dmFields = 0x80000 -bor 0x100000 -bor 0x40000 -bor 0x400000
  $r = [WA]::ChangeDisplaySettingsEx($dev, [ref]$dm, [IntPtr]::Zero, 0, [IntPtr]::Zero)
  return ,@($best[0], $best[1], $r, $best[2])
}


# Display scaling lives in the per-monitor key, encoded as a relative step
# (0 = 100%, each step is +25%; larger text is negative): 200% -> -4.
function Get-DpiKey {
  $root = 'HKCU:\Control Panel\Desktop\PerMonitorSettings'
  if (-not (Test-Path $root)) { return $null }
  $k = Get-ChildItem $root | Select-Object -First 1
  if (-not $k) { return $null }
  return $k.PSPath
}

function Get-DpiPercent {
  $k = Get-DpiKey
  if (-not $k) { return 0 }
  $v = (Get-ItemProperty $k -ErrorAction SilentlyContinue).DpiValue
  if ($null -eq $v) { $v = 0 }
  if ($v -gt 2147483647) { $v = $v - 4294967296 }
  return (4 - $v) * 25
}

function Set-DpiPercent([int]$pct) {
  $k = Get-DpiKey
  if (-not $k) { return -1 }
  $step = 4 - [int]($pct / 25)
  Set-ItemProperty -Path $k -Name DpiValue -Value $step -Type DWord
  Set-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name LogPixels -Value ([int]($pct * 96 / 100)) -Type DWord
  Set-ItemProperty -Path 'HKCU:\Control Panel\Desktop' -Name Win8DpiScaling -Value 1 -Type DWord
  return $pct
}

function J($o, $n) { if ($o.PSObject.Properties.Name -contains $n) { return $o.$n } return $null }
function JQ($s) { return (ConvertTo-Json $s -Compress) }

# make sure the guest-side audio capturer is running (it streams to the viewer
# over TCP 8791).  Started here because the Startup shortcut proved unreliable.
$haveAudio = Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" |
             Where-Object { $_.CommandLine -like '*winvm-audio*' }
# 音频走标准 QEMU audiodev（intel-hda + hda-duplex）后，guest 侧的 WASAPI
# loopback 抓取就不需要了；两路同时开会叠音。保留脚本文件但不启动。
# if (-not $haveAudio -and (Test-Path "C:\winvm-audio.ps1")) {
#   Start-Process powershell -WindowStyle Hidden -ArgumentList '-ExecutionPolicy','Bypass','-File','C:\winvm-audio.ps1'
# }

# 整个服务外面包一层：出错就等 2 秒重来，避免 PowerShell 异常让 agent 彻底消失
# 虚拟机里没有省电的意义：每次登录先把待机/灭屏/休眠/屏保全关掉（幂等，很快）
function Set-NoSleep {
  try {
    powercfg /change monitor-timeout-ac 0
    powercfg /change monitor-timeout-dc 0
    powercfg /change standby-timeout-ac 0
    powercfg /change standby-timeout-dc 0
    powercfg /change disk-timeout-ac 0
    powercfg /change disk-timeout-dc 0
    powercfg /change hibernate-timeout-ac 0
    powercfg /change hibernate-timeout-dc 0
    powercfg /hibernate off
    # 显式把"显示器关闭/进入待机"的 AC/DC 值写成 0：只改 timeout 不够，
    # 某些自定义电源方案会把它覆盖回去
    foreach ($sub in @('SUB_VIDEO VIDEOIDLE', 'SUB_SLEEP STANDBYIDLE', 'SUB_SLEEP HIBERNATEIDLE')) {
      $parts = $sub.Split(' ')
      powercfg /setacvalueindex SCHEME_CURRENT $parts[0] $parts[1] 0
      powercfg /setdcvalueindex SCHEME_CURRENT $parts[0] $parts[1] 0
    }
    powercfg /setactive SCHEME_CURRENT
    # 屏保同样没意义：它会把画面变成黑屏且挡住桌面
    $desk = 'HKCU:\Control Panel\Desktop'
    Set-ItemProperty -Path $desk -Name ScreenSaveActive -Value '0' -Type String
    Set-ItemProperty -Path $desk -Name ScreenSaveTimeOut -Value '0' -Type String
    Set-ItemProperty -Path $desk -Name ScreenSaverIsSecure -Value '0' -Type String
    # 锁屏超时（组策略路径，可能不存在，忽略错误）
    $pol = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\Personalization'
    if (Test-Path $pol) { Set-ItemProperty -Path $pol -Name NoLockScreen -Value 1 -Type DWord }
  } catch {}
}

Set-NoSleep

while ($true) {
 try {
  $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Any, 8790)
  $listener.Start()
  while ($true) {
  $client = $listener.AcceptTcpClient()
  # 关键：客户端不吭声就断开，否则单线程的 Accept 循环会被一个死连接永久堵住
  $client.ReceiveTimeout = 15000
  $stream = $client.GetStream()
  $stream.ReadTimeout = 15000
  $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)
  $writer = New-Object System.IO.StreamWriter($stream, [System.Text.Encoding]::UTF8)
  $writer.AutoFlush = $true
  try {
    while ($true) {
      $line = $reader.ReadLine()
      if ($null -eq $line) { break }
      if ($line.Trim() -eq '') { continue }
      try { $req = $line | ConvertFrom-Json } catch { $writer.WriteLine('{"ok":false,"err":"badjson"}'); continue }
      $cmd = J $req 'cmd'
      $dev = J $req 'dev'; if (-not $dev) { $dev = Get-PrimaryDisplay }
      $resp = '{"ok":false,"err":"unknown"}'
      switch ($cmd) {
        'ping' { $resp = '{"ok":true,"agent":"winvm"}' }
        # 供宿主托盘判断"Windows 是否真的就绪"，比只看端口通不通准得多
        'status' {
          $sid = (Get-Process -Id $PID).SessionId
          $desk = [int][bool](Get-Process explorer -ErrorAction SilentlyContinue)
          $lock = [int][bool](Get-Process LogonUI -ErrorAction SilentlyContinue)
          $up = 0
          try {
            $boot = (Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).LastBootUpTime
            $up = [int]((Get-Date) - $boot).TotalSeconds
          } catch {}
          $resp = '{"ok":true,"session":' + $sid + ',"user":"' + $env:USERNAME +
                  '","desktop":' + $desk + ',"locked":' + $lock + ',"uptime":' + $up + '}'
        }
        'displays' {
          $items = @()
          foreach ($d in (Get-Displays)) {
            $c = Get-Current $d.name
            $items += ('{"name":' + (JQ $d.name) + ',"desc":' + (JQ $d.desc) +
                       ',"active":' + $(if ($d.active) {'true'} else {'false'}) +
                       ',"w":' + $c[0] + ',"h":' + $c[1] + '}')
          }
          $resp = '{"ok":true,"list":[' + ($items -join ',') + ']}'
        }
        'modes' {
          $items = @()
          foreach ($m in (Get-Modes $dev)) { $items += ('[' + $m[0] + ',' + $m[1] + ',' + $m[2] + ']') }
          $resp = '{"ok":true,"dev":' + (JQ $dev) + ',"modes":[' + ($items -join ',') + ']}'
        }
        'getmode' {
          $c = Get-Current $dev
          $resp = '{"ok":true,"dev":' + (JQ $dev) + ',"w":' + $c[0] + ',"h":' + $c[1] + ',"hz":' + $c[2] + '}'
        }
        'setres' {
          $w = [int](J $req 'w'); $h = [int](J $req 'h')
          $r = Set-Res $dev $w $h
          $resp = '{"ok":true,"dev":' + (JQ $dev) + ',"w":' + $r[0] + ',"h":' + $r[1] + ',"hz":' + $r[3] + ',"rc":' + $r[2] + '}'
        }
        'getdpi' {
          $resp = '{"ok":true,"percent":' + (Get-DpiPercent) + '}'
        }
        'setdpi' {
          $pct = [int](J $req 'percent')
          if ($pct -lt 100) { $pct = 100 }
          if ($pct -gt 500) { $pct = 500 }
          $r = Set-DpiPercent $pct
          $resp = '{"ok":true,"percent":' + $r + ',"reload":true}'
        }
        'run' {
          $code = [string](J $req 'code')
          $b64 = [string](J $req 'b64')
          if ($b64) { $code = [System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($b64)) }
          try {
            $out = & ([scriptblock]::Create($code)) 2>&1 | Out-String
          } catch { $out = "ERR: $($_.Exception.Message)" }
          $resp = '{"ok":true,"out":' + (JQ $out) + '}'
        }
        'shutdown' {
          # graceful: let Windows flush and close; the viewer waits for QMP SHUTDOWN
          try {
            Start-Process shutdown -ArgumentList '/s','/t','0','/f' -WindowStyle Hidden
            $resp = '{"ok":true,"action":"shutdown"}'
          } catch { $resp = '{"ok":false,"err":"shutdown"}' }
        }
        'tune' {
          # a VM gains nothing from idle timers; keep the desktop awake
          try {
            Set-NoSleep
            $resp = '{"ok":true,"action":"tune"}'
          } catch { $resp = '{"ok":false,"err":"tune"}'
          }
        }
        'clipget' {
          try { $t = Get-Clipboard -Raw -Format Text } catch { $t = $null }
          if ($null -eq $t) { $t = '' }
          $resp = '{"ok":true,"text":' + (JQ $t) + '}'
        }
        'clipset' {
          $t = [string](J $req 'text')
          try { Set-Clipboard -Value $t; $resp = '{"ok":true}' } catch { $resp = '{"ok":false,"err":"clipset"}' }
        }
      }
      $writer.WriteLine($resp)
    }
  } catch {
  } finally {
    try { $writer.Close() } catch {}
    try { $client.Close() } catch {}
  }
  }
 } catch {
  try { Add-Content -Path 'C:\winvm-agent.log' -Value ((Get-Date).ToString() + ' ' + $_.Exception.Message) } catch {}
  Start-Sleep -Seconds 2
 }
}
