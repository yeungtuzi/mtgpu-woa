# MT H.264 MFT → Sunshine hardware encoding (working)

The MediaTek M1000 vGPU registers a hardware H.264 encoder MFT under the
**NVIDIA H.264 Encoder MFT** CLSID
`{60F44560-5A20-4857-BFEF-D29773CB8040}`
(hardware URL `\\?\PCI#VEN_1ED5&DEV_0381...`, DLL `C:\mtgpu-woa\mtmft13.dll`).

Sunshine's `encoder = mediafoundation` uses FFmpeg's `h264_mf`, which cannot
drive this MFT directly:

* FFmpeg requires hardware MFTs to be **async** (`mfenc.c` errors out
  otherwise), but its async event loop waits for `METransformHaveOutput`
  events that the MT MFT does not emit reliably (it has `OutputDelay: 1`).
* Even when it produces a packet, the MT MFT does not set
  `MFSampleExtension_CleanPoint`, so FFmpeg does not mark the packet as a
  keyframe and Sunshine's probe fails with
  `First packet type is not an IDR frame`.

## The proxy

`mtmft_sync_proxy.cpp` is an async facade registered on the same CLSID. It
creates the real MT MFT (`MTMFT_DLL`) and:

1. exposes `MF_TRANSFORM_ASYNC=1` + `MF_TRANSFORM_ASYNC_UNLOCK` and its own
   `IMFMediaEventGenerator`, mirroring the inner MFT's
   `NeedInput`/`HaveOutput` events (pump thread);
2. after a successful `ProcessInput` queues `NeedInput` + `HaveOutput` hints so
   FFmpeg keeps feeding frames and retries `ProcessOutput` (with a 5 ms
   throttle on `MF_E_TRANSFORM_NEED_MORE_INPUT`);
3. forwards `ICodecAPI` to the inner MFT; on the first input it sets
   `CODECAPI_AVEncVideoForceKeyFrame` and `CODECAPI_AVEncMPVGOPSize = 1`
   (restores GOP 60 after the first sample);
4. inspects each output sample and sets
   `MFSampleExtension_CleanPoint = 1` when it contains an IDR NAL (type 5),
   so FFmpeg emits `AV_PKT_FLAG_KEY` and Sunshine's IDR probe passes;
5. links the C++ runtime **statically** — Sunshine.exe's PATH has no
   `/clangarm64/bin`, and a dynamic `libstdc++` dependency makes the DLL fail
   to load (`could not create MFT`).

The MT MFT itself emits a valid IDR access unit (SPS/PPS/SEI/IDR NAL); only
the CleanPoint attribute was missing.

## Verified

* Sunshine log: `Found H.264 encoder: h264_mf [mediafoundation]`,
  `MFT name: 'NVIDIA H.264 Encoder MFT'`, `New streaming session started`,
  `CLIENT CONNECTED`.
* Moonlight VPlus (host) streams 1080p60 and decodes (VAAPI).
* Host `mthreads-gmi -q`: `Encoder: 25.4%` while streaming (software x264 was
  0%).

## Build / install / restore

```bash
# guest, MSYS2 CLANGARM64
bash build-proxy.sh
```

```powershell
# guest, Administrator
powershell -NoProfile -ExecutionPolicy Bypass -File install-proxy.ps1
# restore original registration:
powershell -NoProfile -ExecutionPolicy Bypass -File install-proxy.ps1 -Restore
```

`install-proxy.ps1` backs up the CLSID to
`C:\mtgpu-woa\mt-mft-clsid-backup.reg` and restarts the `sun-standalone`
scheduled task. The registry layout:

```
HKLM\SOFTWARE\Classes\CLSID\{60F44560-5A20-4857-BFEF-D29773CB8040}\InprocServer32
  (Default)     C:\mtgpu-woa\mtmft_sync_proxy.dll
  MTMFT_DLL     C:\mtgpu-woa\mtmft13.dll
  ThreadingModel Both
```

Sunshine config (`C:\Program Files\Sunshine\config\sunshine.conf`):

```ini
encoder = mediafoundation
min_log_level = info
```

## Files

| file | purpose |
|---|---|
| `mftprobe.cpp` | CoCreateInstance + attributes + input/output type enumeration |
| `mftprobe2.cpp` | LoadLibrary(`mtmft13.dll`) + DllGetClassObject + event generator check |
| `mftasync.cpp` | minimal async harness (proves the MFT encodes when driven correctly) |
| `mtmft_sync_proxy.cpp` | async-facade proxy (the working solution) |
| `build-proxy.sh` | MSYS2 CLANGARM64 build with static runtime |
| `install-proxy.ps1` | register / restore + Sunshine restart |

## Notes / limits

* The proxy adds a small forwarding cost; the 5 ms retry throttle in
  `ProcessOutput` only triggers while the MFT has no output ready.
* GOP is set to 1 for the first input (probe/IDR) and restored to 60 after the
  first sample.
* `mftasync.exe` / probes are diagnostics only; they are not needed at runtime.
