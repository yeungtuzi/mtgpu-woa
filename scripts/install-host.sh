#!/bin/bash
# 宿主侧安装：winvm 启动器 + 托盘控制面板 + 开机自启。
#
#   sudo ./scripts/install-host.sh
#
# 不安装 FreeRDP —— 发行版自带的包通常是 WITH_GFX_H264=OFF 编译的，
# 解不了 AVC444，会花屏。本脚本会检查，并给出构建命令。
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
say() { printf '\033[1m== %s\033[0m\n' "$*"; }
info() { printf '   %s\n' "$*"; }

[ "$(id -u)" = 0 ] || { echo "请用 root 运行"; exit 1; }

say '检查依赖'
missing=()
for pkg in qemu-system-arm python3 xdotool; do
  command -v "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
done
python3 - <<'PY' 2>/dev/null || missing+=("python3-gi + gir1.2-ayatanaappindicator3")
import gi
gi.require_version("Gtk", "3.0")
gi.require_version("AppIndicator3", "0.1")
PY
if [ ${#missing[@]} -gt 0 ]; then
  echo "缺少：${missing[*]}"
  echo "Ubuntu/Debian: apt install qemu-system-arm python3-gi gir1.2-ayatanaappindicator3-0.1 xdotool"
  exit 1
fi
info '依赖齐全'

say '安装启动器'
install -m 755 "$ROOT/bin/winvm" /usr/local/bin/winvm
info '/usr/local/bin/winvm'

say '安装托盘控制面板'
install -d /usr/local/lib/winvm
install -m 755 "$ROOT/host/winvm-tray.py" /usr/local/lib/winvm/winvm-tray.py
install -m 755 "$ROOT/host/winvm-tray-run.sh" /usr/local/bin/winvm-tray-run
install -d /usr/local/lib/winvm/icons
install -m 644 "$ROOT"/host/icons/*.png /usr/local/lib/winvm/icons/
info '/usr/local/bin/winvm-tray-run + 兔子图标'

# 自启：KDE 与 GNOME 都认 XDG autostart
target_user=${SUDO_USER:-$USER}
target_home=$(getent passwd "$target_user" | cut -d: -f6)
install -d -o "$target_user" -g "$target_user" "$target_home/.config/autostart"
install -m 644 -o "$target_user" -g "$target_user" \
  "$ROOT/host/winvm-tray.desktop" "$target_home/.config/autostart/winvm-tray.desktop"
info "$target_home/.config/autostart/winvm-tray.desktop"

say '配置免密 sudo'
# 托盘要能调 sudo winvm start/stop；只放开这一条命令
cat >/etc/sudoers.d/winvm <<EOF
$target_user ALL=(root) NOPASSWD: /usr/local/bin/winvm
EOF
chmod 440 /etc/sudoers.d/winvm
visudo -cf /etc/sudoers.d/winvm >/dev/null && info '/etc/sudoers.d/winvm'

say '安装 RDP 客户端'
# 仓库自带的预编译包：发行版的 freerdp 是 WITH_GFX_H264=OFF，解不了 AVC444
deb=$(ls "$ROOT"/dist/freerdp3-winvm_*.deb 2>/dev/null | head -1)
if [ -n "$deb" ]; then
  info "使用自带包 $(basename "$deb")"
  apt-get remove --purge -y freerdp3-x11 libfreerdp3-3 libfreerdp-client3-3 libwinpr3-3 >/dev/null 2>&1 || true
  DEBIAN_FRONTEND=noninteractive apt-get install -y "$deb" >/dev/null
  lib=$(ldd "$(command -v xfreerdp)" 2>/dev/null | awk '/libfreerdp3/{print $3; exit}')
  if strings "$lib" 2>/dev/null | grep -q 'WITH_GFX_H264=ON'; then
    info '已安装，且确认 WITH_GFX_H264=ON'
  else
    echo '安装后仍未启用 H.264，请检查包的来源'; exit 1
  fi
else
  info 'dist/ 下没有预编译包，检查已有客户端'
fi

client=''
for c in /usr/bin/xfreerdp /usr/local/bin/xfreerdp /home/*/FreeRDP/build/client/X11/xfreerdp; do
  [ -x "$c" ] || continue
  lib=$(ldd "$c" 2>/dev/null | awk '/libfreerdp3/{print $3; exit}')
  strings "$lib" 2>/dev/null | grep -q 'WITH_GFX_H264=ON' && client=$c && break
done
if [ -n "$client" ]; then
  info "使用 $client"
else
  cat <<'MSG'
   没有找到可用的 FreeRDP 客户端。

   发行版包（freerdp3-x11）是 WITH_GFX_H264=OFF 编译的，无法解码 AVC444，
   /gfx 会花屏。请自行构建：

     apt install cmake ninja-build libssl-dev libx11-dev libxext-dev \
                 libxinerama-dev libxcursor-dev libxrandr-dev libxi-dev \
                 libxv-dev libxfixes-dev libavcodec-dev libavutil-dev \
                 libswscale-dev libpulse-dev libcups2-dev libpcsclite-dev \
                 libkrb5-dev libfuse3-dev libsystemd-dev libcairo2-dev libjpeg-dev
     git clone --depth 1 --branch 3.32.1 https://github.com/FreeRDP/FreeRDP
     cd FreeRDP && mkdir build && cd build
     cmake .. -GNinja -DCMAKE_BUILD_TYPE=Release \
       -DWITH_FFMPEG=ON -DWITH_SWSCALE=ON -DWITH_GFX_H264=ON -DWITH_VIDEO_FFMPEG=ON \
       -DWITH_X11=ON -DWITH_CLIENT=ON -DWITH_SERVER=OFF -DWITH_SHADOW=OFF \
       -DWITH_PULSE=ON -DWITH_ALSA=OFF -DWITH_CUPS=ON -DWITH_PCSC=ON \
       -DWITH_KRB5=ON -DWITH_FUSE=ON -DWITH_SYSTEMD=ON -DWITH_WAYLAND=OFF \
       -DWITH_CLIENT_SDL=OFF -DWITH_MANPAGES=OFF -DWITH_SAMPLE=OFF
     ninja -j$(nproc)

   构建完成后用 RDP_CLIENT=/path/to/xfreerdp 指定，或放到 /home/<user>/FreeRDP/build/client/X11/xfreerdp。
MSG
fi

say '配置文件'
if [ ! -f /etc/winvm.conf ]; then
  cat >/etc/winvm.conf <<'EOF'
# winvm 运行配置。不要提交进 git。
MODE=rdp
START_RES=1920x1080
RDP_PORT=3389
RDP_BIND=127.0.0.1
RDP_USER=Administrator
# RDP_PASS=<改成你的密码>
EOF
  chmod 600 /etc/winvm.conf
  info '/etc/winvm.conf 已创建（记得填 RDP_PASS）'
else
  info '/etc/winvm.conf 已存在，保持不动'
fi

say '完成'
cat <<'MSG'
   下一步：
     1. 在 guest 里跑 scripts/install-guest.ps1（需要先装 virtio 驱动，
        见 docs/INSTALL.md）
     2. 填好 /etc/winvm.conf 的 RDP_PASS
     3. sudo winvm start
     4. 托盘会在登录后自动出现；也可手动运行 winvm-tray-run
MSG
