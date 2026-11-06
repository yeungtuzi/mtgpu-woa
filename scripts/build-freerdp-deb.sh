#!/bin/bash
# 把自建的 FreeRDP 打成 deb，用来替代发行版那套 WITH_GFX_H264=OFF 的包。
#
#   sudo ./scripts/build-freerdp-deb.sh
#
# 前提：已经按 docs/INSTALL.md 构建过 /home/lf/FreeRDP/build（或改 SRC 变量），
#       并跑过 cpack -G DEB 生成 freerdp-*.deb。
set -e

SRC=${SRC:-/home/lf/FreeRDP/build}
OUT=${OUT:-/tmp}
WORK=${WORK:-/tmp/debx2}
BASE=$(ls "$SRC"/freerdp-*.deb | head -1)
[ -n "$BASE" ] || { echo "找不到 $SRC/freerdp-*.deb"; exit 1; }

cd "$WORK" 2>/dev/null || { mkdir -p "$WORK"; cd "$WORK"; }
rm -rf root
dpkg-deb -R "$BASE" root
mkdir -p root/usr/lib/aarch64-linux-gnu
for f in root/usr/lib/*.so*; do [ -e "$f" ] && mv "$f" root/usr/lib/aarch64-linux-gnu/; done
rm -rf root/DEBIAN
mkdir -p root/DEBIAN
cat > root/DEBIAN/control <<'EOF'
Package: freerdp3-winvm
Version: 3.32.1+winvm1
Section: x11
Priority: optional
Architecture: arm64
Maintainer: winvm project <noreply@example.invalid>
Installed-Size: 25000
Conflicts: freerdp3-x11, libfreerdp3-3, libfreerdp-client3-3, libwinpr3-3, freerdp3-dev
Replaces: freerdp3-x11, libfreerdp3-3, libfreerdp-client3-3, libwinpr3-3
Provides: freerdp3-x11, libfreerdp3-3, libwinpr3-3
Depends: libc6, libx11-6, libxext6, libxinerama1, libxcursor1, libxrandr2, libxi6, libxv1, libxfixes3, libxrender1, libssl3, libavcodec60, libavutil58, libswscale7, libpulse0, libcups2, libpcsclite1, libkrb5-3, libfuse3-3, libsystemd0, libcairo2, libusb-1.0-0
Description: FreeRDP with H.264/AVC444 enabled (winvm build)
 FreeRDP 3.32.1 built with WITH_FFMPEG=ON, WITH_SWSCALE=ON and
 WITH_GFX_H264=ON.  Distribution builds disable all of these, so the
 RemoteFX/AVC444 graphics pipeline cannot be decoded and /gfx shows garbage.
 .
 This package replaces the distribution's freerdp3 client libraries with a
 build that can actually decode the RDP graphics pipeline.
EOF
dpkg-deb -b root "$OUT/freerdp3-winvm_3.32.1+winvm1_arm64.deb" >/dev/null
echo "--- 打包完成 ---"
dpkg-deb -I "$OUT/freerdp3-winvm_3.32.1+winvm1_arm64.deb" | head -12
ls -lh "$OUT/freerdp3-winvm_3.32.1+winvm1_arm64.deb"
