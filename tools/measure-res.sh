#!/usr/bin/env python3
"""Measure viewer frame rate under a fixed synthetic load, at a given guest
resolution. Used to tell whether the bottleneck is pixel-count bound
(compositing / transport) or render bound."""
import socket, json, time, sys, subprocess

HOSTPORT = 8790

def agent(req, to=180):
    s = socket.create_connection(("127.0.0.1", HOSTPORT), timeout=10)
    s.settimeout(to)
    f = s.makefile("rw", encoding="utf-8")
    f.write(json.dumps(req) + "\n"); f.flush()
    return f.readline().strip()

ANIM = r"""
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$f = New-Object System.Windows.Forms.Form
$f.FormBorderStyle = 'None'
$f.WindowState = 'Maximized'
$f.BackColor = 'Black'
$f.Show()
$g = $f.CreateGraphics()
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$i = 0
while ($sw.ElapsedMilliseconds -lt 9000) {
  $b = New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(255, ($i % 255), (255 - ($i % 255)), 90))
  $g.FillRectangle($b, 0, 0, $f.Width, $f.Height)
  $b.Dispose()
  [Windows.Forms.Application]::DoEvents()
  $i++
}
$f.Close()
"anim_done=$i"
"""

def sample_fps(seconds=6):
    """Read the viewer's perf lines from its log over `seconds`."""
    out = subprocess.run(
        ["ssh", "-o", "ConnectTimeout=8", "-i", "/root/.ssh/id_ed25519_aibook",
         "lf@192.168.100.136",
         "sudo grep -a 'perf:' /tmp/winvm-viewer.log | tail -40"],
        capture_output=True, text=True).stdout.strip().splitlines()
    return out

def main():
    w, h = int(sys.argv[1]), int(sys.argv[2])
    print(f"== guest mode -> {w}x{h} ==")
    print(agent({"cmd": "setres", "w": w, "h": h})[:200])
    time.sleep(3)
    # fire the animation without waiting for it to finish
    s = socket.create_connection(("127.0.0.1", HOSTPORT), timeout=10)
    s.sendall(__import__("json").dumps({"cmd": "run", "code": ANIM}).encode() + b"\n")
    s.close()
    time.sleep(2)
    lines = sample_fps()
    print("\n".join(lines[-8:]))
    time.sleep(9)

if __name__ == "__main__":
    main()
