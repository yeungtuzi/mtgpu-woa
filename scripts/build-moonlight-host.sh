#!/bin/bash
# 构建 Moonlight VPlus（aarch64 Linux），输出到 /opt/mtgpu-woa/moonlight
# 用法：scripts/build-moonlight-host.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="${ROOT:-/opt/mtgpu-woa}"
SRC="$ROOT/src/moonlight-qt"
PREFIX="$ROOT/moonlight"
REPO="${MOONLIGHT_REPO:-https://github.com/qiin2333/moonlight-qt}"
BRANCH="${MOONLIGHT_BRANCH:-master}"

echo "== 依赖 =="
sudo apt-get update -qq
sudo apt-get install -y \
  qt6-base-dev qt6-base-dev-tools qt6-svg-dev \
  libsdl2-dev libsdl2-ttf-dev libopus-dev libssl-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
  libegl1-mesa-dev libgl1-mesa-dev libxkbcommon-dev libpulse-dev \
  build-essential git cmake

echo "== 源码 =="
sudo mkdir -p "$ROOT/src"
sudo chown -R "$(id -u):$(id -g)" "$ROOT"
if [ ! -d "$SRC/.git" ]; then
  git clone -b "$BRANCH" "$REPO" "$SRC"
fi
git -C "$SRC" fetch --all --tags
git -C "$SRC" checkout "$BRANCH"
git -C "$SRC" pull --ff-only || true
git -C "$SRC" submodule sync --recursive
git -C "$SRC" submodule update --init --recursive || true

# VPlus 的 qmdnsengine 是嵌套子模块，浅克隆时经常缺失，显式补齐
if [ ! -e "$SRC/qmdnsengine/qmdnsengine/src/abstractserver.cpp" ] && \
   [ ! -e "$SRC/qmdnsengine/qmdnsengine/src/src/abstractserver.cpp" ]; then
  echo "== 补齐 qmdnsengine 子模块 =="
  rm -rf "$SRC/qmdnsengine/qmdnsengine"
  git clone --depth 1 https://github.com/cgutman/qmdnsengine.git "$SRC/qmdnsengine/qmdnsengine"
fi

echo "== 补丁 =="
PATCH="$HERE/scripts/patches/moonlight-qt/0001-qt6-overlay-opacity-notify.patch"
if [ -f "$PATCH" ]; then
  if git -C "$SRC" apply --check "$PATCH" 2>/dev/null; then
    git -C "$SRC" apply "$PATCH"
    echo "已应用: $PATCH"
  else
    echo "补丁已应用或不可用，跳过: $PATCH"
  fi
fi

echo "== 构建 qmdnsengine（含 moc） =="
( cd "$SRC/qmdnsengine" && /usr/bin/qmake6 -o Makefile qmdnsengine.pro && make -f Makefile.Release )

echo "== 构建 Moonlight =="
cd "$SRC"
qmake6 moonlight-qt.pro
make release -j"$(nproc)"

BIN="$SRC/app/moonlight"
if [ ! -x "$BIN" ]; then
  echo "未找到 $BIN，构建失败" >&2
  exit 1
fi

echo "== 安装到 $PREFIX =="
mkdir -p "$PREFIX/bin"
cp -f "$BIN" "$PREFIX/bin/moonlight"
chmod +x "$PREFIX/bin/moonlight"
ln -sf "$PREFIX/bin/moonlight" "$PREFIX/moonlight" 2>/dev/null || true
echo "完成：$PREFIX/bin/moonlight"
