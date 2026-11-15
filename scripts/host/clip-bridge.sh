#!/usr/bin/env bash
# Host side of the two-way clipboard bridge (xclip <-> HTTP :8765).
# Resolves the active X authorisation file (KDE/Wayland XWayland session).
set -euo pipefail
export DISPLAY="${DISPLAY:-:0}"
if [ -z "${XAUTHORITY:-}" ]; then
  XAUTHORITY=$(ls /run/user/$(id -u)/xauth_* 2>/dev/null | head -1 || true)
  export XAUTHORITY
fi
exec python3 /opt/mtgpu-woa/share/clip-bridge.py
