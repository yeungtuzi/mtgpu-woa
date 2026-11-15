# MT H.264 MFT investigation (guest side)

These tools investigate whether Sunshine's Media Foundation encoder
(`h264_mf` in FFmpeg, encoder name `mediafoundation`) can use the MediaTek
(M1000 vGPU) hardware H.264 encoder.

## What the MT encoder is

The MT driver registers its H.264 encoder under the **NVIDIA H.264 Encoder MFT**
CLSID `{60F44560-5A20-4857-BFEF-D29773CB8040}`. `MFTEnumEx(MFT_ENUM_FLAG_HARDWARE)`
lists it with hardware URL
`\\?\PCI#VEN_1ED5&DEV_0381&SUBSYS_00001ED5&REV_01#...` (VEN_1ED5 = MT).
InprocServer32 points at `C:\mtgpu-woa\mtmft13.dll` (an MT MFT copy).

## Findings (2026-11-15)

* The MFT is **asynchronous**: `MF_TRANSFORM_ASYNC=1`, `MF_LOW_LATENCY=1`.
* Input types: NV12, IYUV, YV12, YUY2 (system memory); output: H264.
* FFmpeg's `h264_mf` hangs on it with `-hw_encoding 1` (frame=0 forever).
  FFmpeg requires hardware MFTs to be async and its async event loop does not
  match this MFT's NeedInput/HaveOutput timing (the MFT has an output delay and
  does not emit HaveOutput reliably after the first input).
* `mftasync.cpp` proves the MFT itself encodes when driven correctly
  (ProcessInput succeeds, the MFT logs `bitstream size: ...`), but it needs a
  caller that tolerates `MF_E_TRANSFORM_NEED_MORE_INPUT`/output delay.
* `mtmft_sync_proxy.cpp` is an async-facade proxy registered on the NVIDIA
  CLSID. It passes FFmpeg's "hardware MFT is async" gate and forwards
  ProcessInput successfully, but FFmpeg still blocks waiting for HaveOutput.
  A proper fix is a Sunshine encoder that drives the MFT directly, or an
  FFmpeg `mfenc` change that polls ProcessOutput after ProcessInput.

## Files

| file | purpose |
|---|---|
| `mftprobe.cpp` | CoCreateInstance + attributes + input/output type enumeration |
| `mftprobe2.cpp` | LoadLibrary(`mtmft13.dll`) + DllGetClassObject + event generator check |
| `mftasync.cpp` | minimal async harness: sets types, streams, ProcessInput, events |
| `mtmft_sync_proxy.cpp` | async-facade proxy DLL (experimental) |

## Build (guest, MSYS2 CLANGARM64)

```bash
export MSYSTEM=CLANGARM64
export PATH=/clangarm64/bin:$PATH
clang++ -std=c++17 -O1 mftasync.cpp -o mftasync.exe -lmfplat -lmfuuid -lole32 -luuid
clang++ -std=c++17 -O2 -shared -o mtmft_sync_proxy.dll mtmft_sync_proxy.cpp \
        -lmfplat -lmfuuid -lole32 -luuid
```

## Proxy registration / rollback (experimental)

```bat
reg export "HKLM\SOFTWARE\Classes\CLSID\{60F44560-5A20-4857-BFEF-D29773CB8040}" C:\mtgpu-woa\mt-mft-clsid-backup.reg /y
reg add "HKLM\SOFTWARE\Classes\CLSID\{60F44560-5A20-4857-BFEF-D29773CB8040}\InprocServer32" /ve /t REG_SZ /d "C:\mtgpu-woa\mtmft_sync_proxy.dll" /f
reg add "HKLM\SOFTWARE\Classes\CLSID\{60F44560-5A20-4857-BFEF-D29773CB8040}\InprocServer32" /v MTMFT_DLL /t REG_SZ /d "C:\mtgpu-woa\mtmft13.dll" /f
```

Restore with:

```bat
reg add "HKLM\SOFTWARE\Classes\CLSID\{60F44560-5A20-4857-BFEF-D29773CB8040}\InprocServer32" /ve /t REG_SZ /d "C:\mtgpu-woa\mtmft13.dll" /f
reg delete "HKLM\SOFTWARE\Classes\CLSID\{60F44560-5A20-4857-BFEF-D29773CB8040}\InprocServer32" /v MTMFT_DLL /f
```
