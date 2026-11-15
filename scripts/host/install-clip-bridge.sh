#!/usr/bin/env bash
# Install the host clipboard bridge as a systemd user service.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
install -d /opt/mtgpu-woa/share
install -m 0755 "$here/clip-bridge.py" /opt/mtgpu-woa/share/clip-bridge.py
install -m 0755 "$here/clip-bridge.sh" /opt/mtgpu-woa/share/clip-bridge.sh
install -d "$HOME/.config/systemd/user"
install -m 0644 "$here/clip-bridge.service" "$HOME/.config/systemd/user/clip-bridge.service"
systemctl --user daemon-reload
systemctl --user enable --now clip-bridge.service
systemctl --user status clip-bridge.service --no-pager | head -6
