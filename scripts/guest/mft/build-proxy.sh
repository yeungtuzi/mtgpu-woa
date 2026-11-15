#!/usr/bin/env bash
# Build the async-facade proxy MFT (mtmft_sync_proxy.dll) on the guest.
# Run from an MSYS2 CLANGARM64 shell (or via bash -lc) in this directory.
#
# IMPORTANT: the C++ runtime is linked statically. Sunshine.exe runs with a
# PATH that does not contain /clangarm64/bin; a dynamic libstdc++ dependency
# makes the DLL fail to load and FFmpeg reports "could not create MFT".
set -euo pipefail
export MSYSTEM=CLANGARM64
export PATH=/clangarm64/bin:$PATH
cd "$(dirname "$0")"

clang++ -std=c++17 -O2 -shared -o mtmft_sync_proxy.dll mtmft_sync_proxy.cpp \
  -lmfplat -lmfuuid -lole32 -luuid -loleaut32 \
  -static-libgcc -static-libstdc++ \
  -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic

echo "built: $(pwd)/mtmft_sync_proxy.dll"
objdump -p mtmft_sync_proxy.dll | grep 'DLL Name'
