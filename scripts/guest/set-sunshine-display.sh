#!/usr/bin/env bash
# Sync the Windows guest display mode with the StreamHub stream resolution/FPS.
# The MT virtual display does not handle client-driven mode changes reliably, so
# Sunshine is configured for an explicit manual resolution/refresh rate; the
# task is restarted only when the target changes.
set -euo pipefail

RES="${1:?usage: set-sunshine-display.sh <width>x<height> <fps>}"
FPS="${2:?usage: set-sunshine-display.sh <width>x<height> <fps>}"

[[ "$RES" =~ ^([0-9]{3,4})x([0-9]{3,4})$ ]] || {
    echo "invalid resolution: $RES (expected WIDTHxHEIGHT)" >&2
    exit 2
}
W="${BASH_REMATCH[1]}"
H="${BASH_REMATCH[2]}"
if (( W < 640 || H < 480 || W > 3840 || H > 2160 )); then
    echo "resolution out of range: ${W}x${H}" >&2
    exit 2
fi
case "$FPS" in
    30|60|120|144|165) ;;
    *) echo "unsupported fps: $FPS" >&2; exit 2 ;;
esac

# The MT virtual display does not expose a usable 30Hz mode. Keep the capture
# target at the selected preset (30/60/120) but use the nearest supported
# display refresh: 60Hz for 30fps, 120Hz above 120fps.
DISPLAY_FPS="$FPS"
if (( FPS < 60 )); then
    DISPLAY_FPS=60
elif (( FPS > 120 )); then
    DISPLAY_FPS=120
fi

TARGET="${W}x${H}@${FPS}cap/display${DISPLAY_FPS}"
STATE="${HOME}/.cache/streamhub-guest-display"
mkdir -p "$(dirname "$STATE")"
if [ -f "$STATE" ] && [ "$(cat "$STATE")" = "$TARGET" ]; then
    exit 0
fi

sudo -n python3 - "$W" "$H" "$FPS" "$DISPLAY_FPS" <<'PY'
import sys, re
sys.path.insert(0, "/home/lf")
from qga_file import read_file, write_file

width, height, fps, display_fps = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
p = r"C:\mtgpu-woa\sunshine-built\config\sunshine.conf"
t = read_file(p).decode("utf-8", "replace")
if not t.endswith("\n"):
    t += "\n"

def setkv(s, key, val):
    if re.search(rf"^{re.escape(key)}\s*=.*$", s, re.M):
        return re.sub(rf"^{re.escape(key)}\s*=.*$", f"{key} = {val}", s, flags=re.M)
    return s + f"{key} = {val}\n"

# Manual mode is deliberate: auto mode is unreliable on the MT virtual display.
t = setkv(t, "dd_resolution_option", "manual")
t = setkv(t, "dd_manual_resolution", f"{width}x{height}")
t = setkv(t, "dd_refresh_rate_option", "manual")
t = setkv(t, "dd_manual_refresh_rate", display_fps)
t = setkv(t, "minimum_fps_target", fps)
write_file(p, t.encode())
print(f"guest display -> {width}x{height}@{display_fps}, capture target {fps}fps")
PY

QA_EXEC="sudo -n python3 /home/lf/qga_exec.py"
SCHTASKS='C:\Windows\System32\schtasks.exe'
$QA_EXEC "$SCHTASKS" /End /TN sun-standalone >/dev/null 2>&1 || true
sleep 1
# The hidden VBS launcher exits immediately; end the Sunshine child explicitly.
$QA_EXEC 'C:\Windows\System32\cmd.exe' /c 'taskkill /IM sunshine.exe /F' >/dev/null 2>&1 || true
sleep 1
$QA_EXEC "$SCHTASKS" /Run /TN sun-standalone >/dev/null 2>&1 || true
for _ in $(seq 1 30); do
    if timeout 1 bash -c "</dev/tcp/127.0.0.1/47989" 2>/dev/null; then
        printf '%s\n' "$TARGET" > "$STATE"
        echo "sunshine ready at $TARGET"
        exit 0
    fi
    sleep 0.5
done
echo "sunshine did not become ready" >&2
exit 1
