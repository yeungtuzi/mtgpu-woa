# Install / restore the MT async-facade proxy MFT on the guest.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File install-proxy.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File install-proxy.ps1 -Restore
#
# The proxy is registered on the NVIDIA H.264 Encoder MFT CLSID
# {60F44560-5A20-4857-BFEF-D29773CB8040}; the original registration is backed
# up to C:\mtgpu-woa\mt-mft-clsid-backup.reg (created once).
param([switch]$Restore)

$ErrorActionPreference = 'Stop'
$Clsid = '{60F44560-5A20-4857-BFEF-D29773CB8040}'
$Key   = "HKLM\SOFTWARE\Classes\CLSID\$Clsid\InprocServer32"
$Root  = 'C:\mtgpu-woa'
$Backup = Join-Path $Root 'mt-mft-clsid-backup.reg'

if (-not (Test-Path $Backup)) {
    & reg.exe export "HKLM\SOFTWARE\Classes\CLSID\$Clsid" $Backup /y | Out-Null
    Write-Host "backup: $Backup"
}

if ($Restore) {
    & reg.exe add $Key /ve /t REG_SZ /d "$Root\mtmft13.dll" /f | Out-Null
    & reg.exe delete $Key /v MTMFT_DLL /f 2>$null | Out-Null
    Write-Host "restored original MT MFT registration (mtmft13.dll)"
} else {
    & reg.exe add $Key /ve /t REG_SZ /d "$Root\mtmft_sync_proxy.dll" /f | Out-Null
    & reg.exe add $Key /v MTMFT_DLL /t REG_SZ /d "$Root\mtmft13.dll" /f | Out-Null
    Write-Host "registered proxy: $Root\mtmft_sync_proxy.dll (inner: $Root\mtmft13.dll)"
}

# Restart Sunshine so it picks up the registration.
Get-Process sunshine -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep 2
Start-ScheduledTask -TaskName 'sun-standalone' -ErrorAction SilentlyContinue
Write-Host "Sunshine restart requested"
