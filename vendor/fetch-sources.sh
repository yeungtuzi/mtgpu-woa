#!/usr/bin/env bash
# Optional source fetch for compliance/maintainers.
# Default release is binary-first; only small/hard-to-find/modified sources live
# in the tree (vendor/src, vendor/patches). Huge system packages stay with the
# user's distro.
#
#   ./vendor/fetch-sources.sh            # small: Sunshine + FreeRDP (skip if present)
#   ./vendor/fetch-sources.sh --moonlight # large: Moonlight V+ source + patches (~168 MB)
#   ./vendor/fetch-sources.sh --qemu      # optional: unmodified QEMU 8.2.2 tarball
#   ./vendor/fetch-sources.sh --system    # FFmpeg/SDL2/libplacebo/Qt6 (very large)
#   ./vendor/fetch-sources.sh --all
#   ./vendor/fetch-sources.sh --dry-run | --list
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${SRC_DIR:-$HERE/src}"
MODE=small
DRY=0
KEEP_TREE="${KEEP_TREE:-0}"

for arg in "$@"; do
  case "$arg" in
    --small) MODE=small ;;
    --moonlight) MODE=moonlight ;;
    --qemu) MODE=qemu ;;
    --system) MODE=system ;;
    --all) MODE=all ;;
    --dry-run) DRY=1 ;;
    --list) MODE=list ;;
    -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

run() {
  if [ "$DRY" = 1 ]; then printf '[dry-run]'; printf ' %q' "$@"; printf '\n'; else "$@"; fi
}
need() { command -v "$1" >/dev/null 2>&1 || { echo "missing command: $1" >&2; exit 1; }; }

fetch_git() {
  local name=$1 url=$2 commit=$3 dir="$SRC/$1"
  if [ ! -d "$dir/.git" ]; then run git clone "$url" "$dir"; fi
  run git -C "$dir" fetch --all --tags --prune
  run git -C "$dir" checkout --force "$commit"
  run git -C "$dir" submodule sync --recursive || true
  run git -C "$dir" submodule update --init --recursive --depth 1 || \
    echo "warning: submodule update failed for $name" >&2
}

apply_patches() {
  local name=$1 dir="$SRC/$1"
  shopt -s nullglob
  for p in "$HERE/patches/$name"/*.patch; do
    if git -C "$dir" apply --check "$p" 2>/dev/null; then run git -C "$dir" apply "$p"
    else echo "warning: patch does not apply: $p" >&2; fi
  done
  shopt -u nullglob
}

archive_dir() {
  local name=$1
  [ -d "$SRC/$name" ] || return 0
  run tar --zstd -cf "$SRC/$name.tar.zst" -C "$SRC" "$name"
  [ "$KEEP_TREE" = 1 ] || run rm -rf "$SRC/$name"
}

fetch_tar() {
  local name=$1 url=$2 ext=$3 out="$SRC/$1.tar.xz"
  [ "$ext" = gz ] && out="$SRC/$1.tar.gz"
  [ "$ext" = zst ] && out="$SRC/$1.tar.zst"
  run curl -fL --retry 3 --connect-timeout 20 -o "$out" "$url"
  case "$ext" in
    xz) run tar -xJf "$out" -C "$SRC" ;;
    gz) run tar -xzf "$out" -C "$SRC" ;;
    zst) run tar --zstd -xf "$out" -C "$SRC" ;;
  esac
  local top
  case "$ext" in
    xz) top="$(tar -tJf "$out" | head -1 | cut -d/ -f1)" ;;
    gz) top="$(tar -tzf "$out" | head -1 | cut -d/ -f1)" ;;
    zst) top="$(tar --zstd -tf "$out" | head -1 | cut -d/ -f1)" ;;
  esac
  if [ -n "$top" ] && [ "$top" != "$name" ] && [ -d "$SRC/$top" ] && [ ! -e "$SRC/$name" ]; then
    run mv "$SRC/$top" "$SRC/$name"
  fi
  archive_dir "$name"
}

small() {
  # These two are shipped in vendor/src; re-fetch only if missing.
  if [ -f "$SRC/sunshine.tar.zst" ] && [ -f "$SRC/freerdp.tar.zst" ]; then
    echo "small sources already present in $SRC"
  else
    fetch_git sunshine https://github.com/LizardByte/Sunshine \
      43b74505bc938e2a044d83b11a1ce13199a75753
    archive_dir sunshine
    fetch_git freerdp https://github.com/FreeRDP/FreeRDP \
      bf217a504e54cc719880c228e82353382cd7d4fa
    archive_dir freerdp
  fi
}

moonlight() {
  fetch_git moonlight-qt https://github.com/qiin2333/moonlight-qt \
    6d176a8d9bcbb124d45563e689b593886d05d925
  apply_patches moonlight-qt
  archive_dir moonlight-qt
}

qemu() {
  echo "note: QEMU is unmodified and normally installed from the distro; not bundled." >&2
  fetch_tar qemu-8.2.2 https://download.qemu.org/qemu-8.2.2.tar.xz xz
}

system() {
  fetch_tar ffmpeg-6.1.1 https://ffmpeg.org/releases/ffmpeg-6.1.1.tar.xz xz
  fetch_tar SDL2-2.30.0 \
    https://github.com/libsdl-org/SDL/releases/download/release-2.30.0/SDL2-2.30.0.tar.gz gz
  fetch_git libplacebo https://code.videolan.org/videolan/libplacebo v6.338.2
  archive_dir libplacebo
  fetch_tar qt-everywhere-src-6.4.2 \
    https://download.qt.io/official_releases/qt/6.4/6.4.2/single/qt-everywhere-src-6.4.2.tar.xz xz
  echo "note: /opt/kde6 (Qt 6.11.3/KDE) sources are per-repo on code.qt.io / invent.kde.org; see SOURCES.md" >&2
}

list() { sed -n '/^| 组件 /,/^$/p' "$HERE/SOURCES.md"; }

need git; need curl; need tar
mkdir -p "$SRC"

case "$MODE" in
  small) small ;;
  moonlight) moonlight ;;
  qemu) qemu ;;
  system) system ;;
  all) small; moonlight; qemu; system ;;
  list) list ;;
esac

if [ "$DRY" = 0 ]; then
  ( cd "$SRC" && sha256sum ./*.tar.* > SHA256SUMS 2>/dev/null || true )
  ls -lh "$SRC"/*.tar.* 2>/dev/null | tail -20 || true
fi
