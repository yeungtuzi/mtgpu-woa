# Bidirectional clipboard sync (guest, interactive session) using WinForms + curl.exe.
$ErrorActionPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.Windows.Forms
$log = 'C:\mtgpu-woa\clip-sync2.log'
$stateFile = 'C:\mtgpu-woa\clip-sync2-state.txt'
$tmp = 'C:\mtgpu-woa\clip-post.txt'
$utf8 = New-Object System.Text.UTF8Encoding($false)
$last = ''
if (Test-Path $stateFile) { $last = ([string](Get-Content $stateFile -Raw)).TrimEnd([char[]]"`r`n") }
while ($true) {
    $g = [System.Windows.Forms.Clipboard]::GetText()
    if ($null -eq $g) { $g = '' }
    $g = ([string]$g).TrimEnd([char[]]"`r`n")
    $h = (& curl.exe -s --max-time 3 'http://10.0.2.2:8765/clip' 2>$null) -join "`n"
    $h = ([string]$h).TrimEnd([char[]]"`r`n")
    if ($last -eq '' -and $h -ne '') {
        # First run: host clipboard wins, so a restart cannot clobber it.
        [System.Windows.Forms.Clipboard]::SetText($h)
        $last = $h
        [IO.File]::WriteAllText($stateFile, $h, $utf8)
        Add-Content -Path $log -Value ("INIT-APPLY " + $h)
        Start-Sleep -Milliseconds 1500
        continue
    }
    if ($g -ne '' -and $g -ne $h -and $g -ne $last) {
        [IO.File]::WriteAllText($tmp, $g, $utf8)
        & curl.exe -s --max-time 3 -X POST --data-binary "@$tmp" -H 'Content-Type: text/plain; charset=utf-8' 'http://10.0.2.2:8765/clip' 2>$null | Out-Null
        $last = $g
        [IO.File]::WriteAllText($stateFile, $g, $utf8)
        Add-Content -Path $log -Value ("PUSH " + $g)
    } elseif ($h -ne '' -and $h -ne $g -and $h -ne $last) {
        [System.Windows.Forms.Clipboard]::SetText($h)
        $last = $h
        [IO.File]::WriteAllText($stateFile, $h, $utf8)
        Add-Content -Path $log -Value ("APPLY " + $h)
    }
    Start-Sleep -Milliseconds 1500
}
