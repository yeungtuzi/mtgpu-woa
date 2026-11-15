# Upstream Sunshine source build (guest, Windows 11 ARM64)

This is the build that produced the **running** Sunshine instance on the guest
(2026-11-15): `C:\mtgpu-woa\src\sunshine\build\sunshine.exe` (source build,
MSYS2 CLANGARM64), serving Moonlight with `encoder = mediafoundation` through
the MT async-facade proxy MFT (`scripts/guest/mft/`).

## Prerequisites (guest)

* MSYS2 at `C:\msys64` with the CLANGARM64 environment (`pacman -Syu`, toolchain).
* Node.js **Windows ARM64** unpacked to `C:\mtgpu-woa\node` (v22.11.0 used);
  Sunshine's Web UI build needs `npm`.
* The CLANGARM64 packages used by upstream (boost, cppwinrt, curl-winssl,
  miniupnpc, onevpl, openssl, opus, qt6-static, cmake, ninja, git).

## Build

```bash
# guest, MSYS2 CLANGARM64, from C:\mtgpu-woa (script clones into src/sunshine)
bash /c/mtgpu-woa/build-upstream.sh
```

Key points learned:

* `-DSUNSHINE_ENABLE_TRAY=OFF -DSUNSHINE_REQUIRE_TRAY=OFF` avoids the MinHook
  requirement (there is no CLANGARM64 MinHook package).
* `ninja -j2`: with 8 GB RAM, higher parallelism crashes clang
  (`The paging file is too small`, 0x5AF).
* The Web UI step (`vite build`) can fail on ARM64 with
  `Cannot find native binding` (rolldown optional dependency bug in npm).
  Workaround that is in use: copy `assets/web` from the official install into
  `build\assets\web`; the binary itself is complete.
* The source-built exe uses its own config dir (`build\config`). Copy the
  official `C:\Program Files\Sunshine\config\*` there so the existing Moonlight
  pairing certificates continue to work.

## Run

```powershell
# task used in this project (AtLogOn, Administrator, interactive):
#   C:\mtgpu-woa\src\sunshine\build\sunshine.exe
#   working dir: C:\mtgpu-woa\src\sunshine\build
```

Verified: `Found H.264 encoder: h264_mf [mediafoundation]`,
`MFT name: 'NVIDIA H.264 Encoder MFT'`, 1080p60 stream with
`mthreads-gmi -q` Encoder ≈25%, audio `[F32 48000 2.0]`.

## Rollback

Point the `sun-standalone` task back to
`C:\Program Files\Sunshine\Sunshine.exe` (working dir `C:\Program Files\Sunshine`)
and restart it; the official install is untouched.
