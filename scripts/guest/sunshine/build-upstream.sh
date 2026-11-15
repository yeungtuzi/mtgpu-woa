#!/usr/bin/env bash
set -o pipefail
export MSYSTEM=CLANGARM64
export PATH=/clangarm64/bin:/c/mtgpu-woa/node:$PATH
cd /c/mtgpu-woa || exit 1
mkdir -p src
if [ ! -d src/sunshine/.git ]; then
  echo "== clone =="
  git clone --depth 1 --recurse-submodules --shallow-submodules \
    https://github.com/LizardByte/Sunshine.git src/sunshine || exit 10
fi
cd src/sunshine || exit 11
echo "== submodules =="
git submodule update --init --recursive --depth 1 || exit 12
echo "== cmake =="
cmake -B build -G Ninja -S . \
  -DSUNSHINE_ENABLE_TRAY=OFF -DSUNSHINE_REQUIRE_TRAY=OFF \
  -DCUDA_FAIL_ON_MISSING=OFF -DBUILD_TESTS=OFF -DBUILD_DOCS=OFF || exit 13
echo "== ninja =="
ninja -C build -j2 || exit 14
ls -l build/sunshine.exe && echo BUILD_OK
