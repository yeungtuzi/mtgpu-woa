#!/usr/bin/env bash
# Build StreamHub Native against the session's Qt (/opt/kde6 when present) and
# install it under /opt/mtgpu-woa/streamhub-native.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
src="${STREAMHUB_NATIVE_SRC:-$here/../apps/streamhub-native}"
[ -d "$src" ] || src="$here/apps/streamhub-native"
build="${BUILD_DIR:-$HOME/build/streamhub-native-native}"
dest=/opt/mtgpu-woa/streamhub-native

if [ -f /opt/kde6/env.sh ]; then
    # shellcheck disable=SC1091
    source /opt/kde6/env.sh
fi

cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "$build"

sudo install -d "$dest/bin"
sudo install -m755 "$build/streamhub-native" "$dest/bin/streamhub-native"
sudo rm -rf "$dest/bin/qml"
sudo cp -a "$src/qml" "$src/icon.svg" "$dest/bin/"
sudo chown -R "$(id -u):$(id -g)" "$dest"

cat > "$dest/streamhub-native.sh" <<'SH'
#!/usr/bin/env bash
# Launcher: use the desktop session's Qt (KDE6 prefix) and prefer Wayland when
# the session provides it, so Qt reads the compositor's scale factor (HiDPI).
# Falls back to xcb automatically (QT_QPA_PLATFORM="wayland;xcb").
set -euo pipefail
uid=$(id -u)
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$uid}"
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && [ -S "$XDG_RUNTIME_DIR/bus" ]; then
    export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
fi
if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    for sock in "$XDG_RUNTIME_DIR"/wayland-*; do
        [ -S "$sock" ] || continue
        export WAYLAND_DISPLAY="$(basename "$sock")"
        break
    done
fi
if [ -n "${WAYLAND_DISPLAY:-}" ]; then
    export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-wayland;xcb}"
fi
if [ -f /opt/kde6/env.sh ]; then
    # shellcheck disable=SC1091
    source /opt/kde6/env.sh
fi
exec /opt/mtgpu-woa/streamhub-native/bin/streamhub-native "$@"
SH
chmod +x "$dest/streamhub-native.sh"

install -d "$HOME/.local/share/applications"
cat > "$HOME/.local/share/applications/streamhub-native.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=StreamHub
Comment=Native control panel for the mtgpu-woa Moonlight/Sunshine stack
Exec=$dest/streamhub-native.sh
Icon=$dest/bin/icon.svg
Terminal=false
Categories=Network;Game;Qt;
DESKTOP

echo "installed: $dest"
echo "launcher:  $dest/streamhub-native.sh"
