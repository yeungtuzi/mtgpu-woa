#!/usr/bin/env bash
# Install the host clipboard bridge: scripts + KDE/GNOME autostart entry.
# (systemd --user is not available in every ssh/desktop setup, so autostart
# is the reliable path here.)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
install -d /opt/mtgpu-woa/share
install -m 0755 "$here/clip-bridge.py" /opt/mtgpu-woa/share/clip-bridge.py
install -m 0755 "$here/clip-bridge.sh" /opt/mtgpu-woa/share/clip-bridge.sh
install -d "$HOME/.config/autostart"
install -m 0644 "$here/clip-bridge.desktop" "$HOME/.config/autostart/clip-bridge.desktop"
# start it now (idempotent)
if ! pgrep -f '/opt/mtgpu-woa/share/clip-bridge.py' >/dev/null; then
  setsid /opt/mtgpu-woa/share/clip-bridge.sh </dev/null >/opt/mtgpu-woa/share/clip-bridge.log 2>&1 &
fi
sleep 1
curl -s --max-time 3 http://127.0.0.1:8765/clip && echo " <- clipboard bridge OK"
