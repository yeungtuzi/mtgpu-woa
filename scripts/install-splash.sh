#!/bin/bash
# Install the "aibook" Plasma splash screen (System Settings → 全局主题 → 欢迎画面).
#
#   ./scripts/install-splash.sh            # per-user: ~/.local/share/plasma/look-and-feel
#   sudo ./scripts/install-splash.sh --system
#
# Then pick it in 系统设置 → 全局主题 → 欢迎画面 (Splash Screen).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
PKG="org.aibook.splash"
SRC="$ROOT/share/plasma/look-and-feel/$PKG"

if [ "${1:-}" = "--system" ]; then
    [ "$(id -u)" = 0 ] || { echo "use sudo for --system" >&2; exit 1; }
    data=$(kf6-config --path data 2>/dev/null | cut -d: -f1 || true)
    [ -n "$data" ] || data=/usr/share
    # this machine installs KDE under /opt/kde6
    case "$data" in
        /usr/share) [ -d /opt/kde6/share/plasma ] && data=/opt/kde6/share ;;
    esac
    dest="$data/plasma/look-and-feel/$PKG"
    install -d "$dest"
    cp -a "$SRC/." "$dest/"
    echo "installed: $dest"
else
    dest="$HOME/.local/share/plasma/look-and-feel/$PKG"
    install -d "$dest"
    cp -a "$SRC/." "$dest/"
    echo "installed: $dest"
fi

echo "现在打开 系统设置 → 全局主题 → 欢迎画面，选择 “aibook”。"
