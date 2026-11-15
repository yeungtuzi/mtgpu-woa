#!/usr/bin/env bash
# Set the guest Sunshine display refresh/capture target to the requested FPS.
# dd_refresh_rate_option=auto does not work on this MT virtual display, so we
# write the manual rate and restart Sunshine when the target changes.
set -euo pipefail

FPS="${1:?usage: set-sunshine-fps.sh <30|60|120>}"
case "$FPS" in 30|60|120|144|165) ;; *) echo "unsupported fps: $FPS" >&2; exit 2 ;; esac
STATE="${HOME}/.cache/streamhub-guest-fps"
mkdir -p "$(dirname "$STATE")"
if [ -f "$STATE" ] && [ "$(cat "$STATE")" = "$FPS" ]; then
    exit 0
fi

sudo -n python3 - "$FPS" <<'PY'
import sys, re
sys.path.insert(0, "/home/lf")
from qga_file import read_file, write_file
fps = sys.argv[1]
p = r"C:\mtgpu-woa\sunshine-built\config\sunshine.conf"
t = read_file(p).decode("utf-8", "replace")
if not t.endswith("\n"):
    t += "\n"
def setkv(s, key, val):
    if re.search(rf"^{re.escape(key)}\s*=.*$", s, re.M):
        return re.sub(rf"^{re.escape(key)}\s*=.*$", f"{key} = {val}", s, flags=re.M)
    return s + f"{key} = {val}\n"
t = setkv(t, "dd_refresh_rate_option", "manual")
t = setkv(t, "dd_manual_refresh_rate", fps)
t = setkv(t, "minimum_fps_target", fps)
write_file(p, t.encode())
print("guest display refresh ->", fps)
PY

QA_EXEC="sudo -n python3 /home/lf/qga_exec.py"
SCHTASKS='C:\Windows\System32\schtasks.exe'
$QA_EXEC "$SCHTASKS" /End /TN sun-standalone >/dev/null 2>&1 || true
sleep 1
# With the hidden VBS launcher the task action exits immediately, so End alone
# may not stop the Sunshine child; terminate it explicitly before relaunching.
$QA_EXEC 'C:\Windows\System32\cmd.exe' /c 'taskkill /IM sunshine.exe /F' >/dev/null 2>&1 || true
sleep 1
$QA_EXEC "$SCHTASKS" /Run /TN sun-standalone >/dev/null 2>&1 || true
for _ in $(seq 1 30); do
    if timeout 1 bash -c "</dev/tcp/127.0.0.1/47989" 2>/dev/null; then
        echo "$FPS" > "$STATE"
        echo "sunshine ready at ${FPS}fps"
        exit 0
    fi
    sleep 0.5
done
echo "sunshine did not become ready" >&2
exit 1
