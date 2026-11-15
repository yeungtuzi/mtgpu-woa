# Sunshine ARM64 build fixes and 2-second encode latency

`0001-arm64-build-and-2s-encode-latency.patch` is generated from the Windows guest
Foundation Sunshine tree at `C:\mtgpu-woa\src\foundation-src` (build identity
`bbf9418bc138210dfeaf24fc01bc62de6cb22743`, with local fixes). It is the patch that
produced the working ARM64 `sunshine.exe` used by StreamHub.

## Encode latency fallback

The MediaTek async MFT does not reliably preserve the input PTS. Sunshine's
`video::encode_avcodec()` therefore cannot match `av_packet->pts` to the capture
timestamp and leaves `packet->frame_timestamp` empty. Sunshine then sends
`frame_processing_latency = 0`, so Moonlight omits the `Encode` field entirely.

The patch records the `avcodec_send_frame()` → `avcodec_receive_packet()` duration
in a 2-second rolling window. When the PTS lookup fails, it uses
`now - average_encode_duration` as the frame timestamp. This is an accepted
approximation: it measures the encoder call, **not** the complete capture-to-send
queue. It is not directly comparable to Sunshine's original host-processing
latency, which also includes the capture/display queue.

## ARM64 compile fixes

A clean ARM64 build from this tree otherwise fails on several x86_64/Windows
assumptions:

- AMF C++ headers are included at global scope, not inside `config::amd`.
- `std::atomic<std::shared_ptr<T>>` is replaced with small mutex holders on the
  MSYS2 libc++ version.
- `std::chrono::clock_cast` is replaced by a file_clock/system_clock offset.
- `to_string()` calls on `file_time_type::rep` are cast to `long long`.
- The Steam audio driver has an ARM64 path placeholder.
- `misc.h` includes `<vector>` itself.
- The Windows crash handler is stubbed out for ARM64 because its unwinder uses
  x64 `Rip`/`Rsp`; guest crash reports (not streaming) are therefore disabled on
  ARM64.

## Build prerequisites (guest)

- MSYS2 CLANGARM64, CMake, Ninja.
- `third-party/nvenc-headers` needs the versioned include directory exposed as
  `third-party/nvenc-headers/ffnvcodec` (for example a junction to
  `1301/include/ffnvcodec`).
- The resulting exe dynamically imports `libiconv-2.dll` (and `libcharset-1.dll`
  if present). Copy them next to `sunshine.exe`, or link iconv statically.

## Verification

With a low-latency stream the StreamHub stats endpoint returns a complete set:

```json
{"encode":12.5,"render":8.0,"decode":0.46,"network":7.0,
 "latency":28.0,"missing":[]}
```

Moonlight's stats log then contains the `Encode **12.5**ms` segment.
