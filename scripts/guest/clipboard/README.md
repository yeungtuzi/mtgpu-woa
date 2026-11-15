# Two-way clipboard bridge (host <-> guest)

Upstream Sunshine/Moonlight only supports **client -> host** clipboard (and
only manually via Moonlight's `Ctrl+Alt+Shift+V`); the bidirectional clipboard
of Moonlight V+ is a **Foundation Sunshine** extension. Because Foundation
Sunshine does not build/run on this ARM64 MT platform, this small bridge
provides two-way text clipboard independently of the streaming protocol.

## Layout

* Host (aibook): `scripts/host/clip-bridge.py` (xclip <-> HTTP `:8765`),
  installed by `scripts/host/install-clip-bridge.sh` as a systemd **user**
  service (`~/.config/systemd/user/clip-bridge.service`).
* Guest: `scripts/guest/clipboard/clip-sync2.ps1` polls the bridge every
  1.5 s using `System.Windows.Forms.Clipboard` and `curl.exe` (PowerShell's
  `Invoke-WebRequest` hangs in the interactive task context).

## Guest setup (Administrator, on the guest)

```powershell
# clip-sync2.ps1 is served from the host share at http://10.0.2.2:8000/
curl.exe -s -o C:\mtgpu-woa\clip-sync2.ps1 http://10.0.2.2:8000/clip-sync2.ps1
$act = New-ScheduledTaskAction -Execute 'powershell.exe' `
  -Argument '-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File C:\mtgpu-woa\clip-sync2.ps1'
$pr  = New-ScheduledTaskPrincipal -UserId 'Administrator' -LogonType Interactive -RunLevel Highest
$trg = New-ScheduledTaskTrigger -AtLogOn -User 'Administrator'
Register-ScheduledTask -TaskName 'clip-sync2' -Action $act -Principal $pr -Trigger $trg -Force
Start-ScheduledTask -TaskName 'clip-sync2'
```

## Behaviour / caveats

* First run: **host clipboard wins** (applied to the guest) so restarting the
  agent cannot clobber the host clipboard.
* Text only (no images), ~1.5 s latency.
* The channel is QEMU user-net `10.0.2.2:8765` (host loopback); it is not
  encrypted and is reachable from the guest only.
* Verified both directions:
  - host -> guest: set `bi-h2g-1` on the host, guest clipboard read it back;
  - guest -> host: set `bi-g2c-1` on the guest, `xclip -o` on the host showed
    it (no BOM).
