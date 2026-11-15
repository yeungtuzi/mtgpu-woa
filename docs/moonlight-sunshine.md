# Moonlight / Sunshine 串流方案

> 状态：进行中（2026-11-15）。本文记录从 RDP 切换到 Moonlight/Sunshine 的原因、
> 架构、源码构建方式与回滚路径。

## 1. 为什么放弃 RDP 硬件编码

RDP 的 H.264 硬件编码不是“有硬件 MFT 就会用”，而是与 **AVC444（全屏 4:4:4）模式强绑定**：

- 微软 AVD 文档要求同时启用「配置远程桌面连接的 H.264/AVC 硬件编码」和
  「为远程桌面连接设置 H.264/AVC 444 图形模式的优先级」两个策略，硬件帧编码才会生效。
- Superuser 的 NVIDIA A2 案例明确：客户端若未声明 AVC444 能力（只能 AVC420），
  RDP 服务器**永远不会**使用硬件编码；且需要 Maxwell GM107+ 这类支持 YUV444 编码的 GPU。
- MSDN 论坛同题结论：「硬件编码只支持 AVC444，不支持 AVC420」。

本项目的客户端（macOS Windows App / FreeRDP）不声明 AVC444，因此 RDP 侧：
事件 170 从不出现、`RdpAvenc` 记录为 0、VPU 恒 0。即使把 MT VPU 封装成硬件 MFT 并注册
`MFT_ENUM_HARDWARE_URL`，`MFTEnum2(LUID)` 也不会把它与 M1000 关联（厂商 MFT 由显示驱动包
通过 INF/HKR 注册，用户态注册无法复现）。

结论：在“不使用 AVC444”的约束下，RDP 没有硬件编码路径。改用 Moonlight/Sunshine。

## 2. 架构

```
macOS / Linux 客户端                 Ubuntu 24.04 aarch64 宿主 (aibook)
┌────────────────────┐              ┌─────────────────────────────────────┐
│ Moonlight VPlus    │  LAN/UDP     │ QEMU Windows 11 ARM64 guest          │
│ (源码构建, 可改 UI)│◄────────────►│  Sunshine (Foundation, 源码构建)     │
└────────────────────┘              │   └─ FFmpeg h264_mf (Media Foundation)│
                                    │        └─ MT MFT (mtmft13.dll)       │
                                    │             └─ mtencodeapi64 (VPU)   │
                                    └─────────────────────────────────────┘
```

- **编码**：Sunshine 在 Windows 上用 FFmpeg 的 `h264_mf`（Media Foundation）编码器；
  `h264_mf` 通过 `MFTEnumEx(MFT_ENUM_FLAG_HARDWARE)` 枚举，**不需要 MFTEnum2 的 LUID 关联**，
  因此能直接选中我们已注册的 MT MFT。
- **客户端**：Moonlight VPlus（qiin2333/moonlight-qt）源码构建，便于后续改 UI；
  与 Foundation Sunshine 配套时可获得双向剪贴板、高品质麦克风等扩展。
- **主机访问**：guest 控制台会话（自动登录）由 Sunshine 捕获；宿主托盘面板改为拉起 Moonlight。
  RDP 仅保留作管理用途。

## 3. 目录约定

| 位置 | 内容 |
|---|---|
| 宿主 `/opt/mtgpu-woa/src/moonlight-qt` | Moonlight VPlus 源码 |
| 宿主 `/opt/mtgpu-woa/moonlight` | Moonlight 构建产物/安装前缀 |
| 宿主 `/opt/mtgpu-woa/src/sunshine` | Foundation Sunshine 源码（guest 侧同源） |
| guest `C:\mtgpu-woa\src\foundation-sunshine` | Sunshine 源码/构建目录 |
| guest `C:\Program Files\Sunshine` | Sunshine 安装目录（服务 + 配置） |

## 4. 源码构建

### 4.1 宿主：Moonlight VPlus（aarch64）

```bash
scripts/build-moonlight-host.sh
```

脚本会：安装 Qt6/SDL2/FFmpeg/Opus 依赖 → 克隆 `qiin2333/moonlight-qt`（含子模块）
→ `qmake6 moonlight-qt.pro && make release` → 安装到 `/opt/mtgpu-woa/moonlight`。

### 4.2 guest：Foundation Sunshine（Windows ARM64）

在 guest 内以管理员运行：

```powershell
scripts/build-sunshine-guest.ps1
```

脚本会：下载并静默安装 MSYS2 ARM64（`C:\msys64`）→ 在 `CLANGARM64` 环境安装
`mingw-w64-clang-aarch64-*` 依赖 → 克隆 `AlkaidLab/foundation-sunshine`（含子模块）
→ CMake/Ninja 构建。

## 5. 运行

1. guest 自动登录（`Winlogon\AutoAdminLogon=1` + `DefaultPassword`）到控制台会话。
2. `SunshineService` 自启动，Sunshine 在控制台会话捕获桌面。
3. 宿主托盘面板调用 Moonlight，连接 guest 的 Sunshine（首次需在 Sunshine Web UI 输入配对 PIN）。
4. 验证：`mthreads-gmi -q` 的 `Encoder` 占用 > 0；Moonlight 性能叠加层显示硬件编码。

## 6. 回滚

- QEMU 磁盘快照：`pre-moonlight-20261115`（`qemu-img snapshot -l /home/lf/woa-hist.qcow2`）。
- 回滚：关闭 guest → `sudo qemu-img snapshot -a pre-moonlight-20261115 /home/lf/woa-hist.qcow2`
  → `sudo winvm start`。
- 已安装的 Sunshine 服务/注册表项可用快照整体回退；用户态文件均在 `C:\mtgpu-woa` 与
  `C:\Program Files\Sunshine`，可单独删除。

## 7. 待办

- [x] 宿主 Moonlight VPlus 源码构建（产物 /opt/mtgpu-woa/moonlight/bin/moonlight）
- [ ] guest Foundation Sunshine ARM64 源码构建（MSYS2 CLANGARM64，进行中）
- [ ] Sunshine 配置 `encoder = mediafoundation`，验证 h264_mf 选中 MT MFT
- [ ] 托盘面板从 xfreerdp 切换到 Moonlight
- [ ] 双向音频/剪贴板联调
- [ ] 源码/脚本纳入仓库发布

## 当前状态（2026-11-15 16:20）

### 已打通
- 宿主 Moonlight VPlus 6.2.84 源码构建完成：`/opt/mtgpu-woa/moonlight/bin/moonlight`
- guest Sunshine ARM64 2026.914 以**服务模式**运行（SunshineService），Web UI `https://127.0.0.1:47990`
  （账号 `admin` / `Winvm!2026`，可用 `curl -k -u admin:... https://127.0.0.1:47990/api/pin` 管理配对）
- 配对完成：`moonlight pair 127.0.0.1 --pin 1234`，应用列表含 `Desktop` / `Steam Big Picture`
- 串流验证：1080p60 / 20 Mbps，宿主 VAAPI 硬解正常，guest 侧音频采集 `F32 48000 2.0`
- `winvm` 已加 Sunshine 端口转发（TCP 47984/47989/47990/48010，UDP 47998/47999/48000/48002/48010），
  `/etc/winvm.conf` 配置 `MODE=rdp` + `CLIENT=moonlight`，tray 的“启动/显示”会拉起 Moonlight

### 已知限制 / 阻塞
- **编码器是 software（libx264），MT VPU 硬件编码未打通**：
  - MT 的 H.264 MFT 注册在 NVIDIA CLSID 下，实测 `MF_TRANSFORM_ASYNC=1`（异步），
    输入 NV12/IYUV/YV12/YUY2、输出 H264；FFmpeg `h264_mf`（Sunshine 的 mediafoundation 路径）
    在 `-hw_encoding 1` 下永久挂起（frame=0）
  - 同步代理 MFT 会被 FFmpeg 以 “hardware MFT is not async” 拒绝；异步门面代理能通过检查并
    `ProcessInput` 成功，但 MT MFT 的 HaveOutput 事件时序与 FFmpeg 异步状态机不匹配，仍会卡住
  - 调研与实验代码：`scripts/guest/mft/`（probe / async harness / 代理 + README）
  - 可行后续：给 Sunshine 写原生 MT MFT 编码器（直接按 async 事件驱动，参考 `mftasync.cpp`），
    或给 FFmpeg `libavcodec/mfenc.c` 打补丁（ProcessInput 后轮询 ProcessOutput 而不是死等 HaveOutput）
- Foundation Sunshine（双向剪贴板 + 麦克风）在 ARM64 上源码构建失败：缺少 MinHook（CLANGARM64 无包），
  且其编码器表只有 nvenc/quicksync/amdvce/software，没有 mediafoundation；暂不采用
- upstream Sunshine 的剪贴板只支持 client→host；**双向剪贴板**需要 Foundation 或额外桥接
- 麦克风：upstream 2026 版已有 `mic_wasapi_t`，需要在 Moonlight VPlus 里开启麦克风实测

### 回滚
- 磁盘快照 `pre-moonlight-20261115`（`sudo qemu-img snapshot -l /home/lf/woa-hist.qcow2`）
- MFT 注册备份 `C:\mtgpu-woa\mt-mft-clsid-backup.reg`，当前已恢复为 `C:\mtgpu-woa\mtmft13.dll`
- Sunshine 配置 `C:\Program Files\Sunshine\config\sunshine.conf`（当前 `encoder = software`）
- 旧的 `config.bak` 保留在 `C:\Program Files\Sunshine\config.bak`

## VPU 硬件编码打通（2026-11-15 16:40）

**结论：Sunshine 已通过异步门面代理使用 MT VPU 的硬件 H.264 编码器。**

- MT 的 H.264 MFT 注册在 NVIDIA CLSID `{60F44560-...}` 下（`C:\mtgpu-woa\mtmft13.dll`），
  是 async MFT；FFmpeg `h264_mf` 的异步事件循环与它的输出延迟不匹配，且它不给输出样本打
  `MFSampleExtension_CleanPoint`（Sunshine 探测因此报 “First packet type is not an IDR frame”）。
- 解决：`scripts/guest/mft/mtmft_sync_proxy.cpp` 注册为同一 CLSID 的异步门面：
  - 暴露 `MF_TRANSFORM_ASYNC=1` + `IMFMediaEventGenerator`，泵线程镜像内层 MFT 事件；
  - `ProcessInput` 成功后补 `NeedInput`/`HaveOutput` 提示，`NEED_MORE_INPUT` 时 5 ms 后重试；
  - 转发 `ICodecAPI`，首帧强制 keyframe / GOP=1，之后恢复 GOP=60；
  - 输出样本含 IDR NAL 时补 `MFSampleExtension_CleanPoint`；
  - **静态链接 C++ 运行时**（Sunshine 的 PATH 没有 `/clangarm64/bin`，动态依赖会导致 DLL 加载失败）。
- 证据：
  - Sunshine 日志：`Found H.264 encoder: h264_mf [mediafoundation]`、
    `MFT name: 'NVIDIA H.264 Encoder MFT'`、`New streaming session started`、`CLIENT CONNECTED`
  - 宿主 `mthreads-gmi -q`：串流时 `Encoder: 25.4%`（software x264 时为 0%）
  - Moonlight VPlus 1080p60 串流、VAAPI 解码正常，音频 `[F32 48000 2.0]` 正常
- 运行方式：`C:\Program Files\Sunshine\config\sunshine.conf` 设 `encoder = mediafoundation`；
  Sunshine 由计划任务 `sun-standalone`（Administrator，AtLogOn，Highest）启动；
  代理注册/恢复：`scripts/guest/mft/install-proxy.ps1`（`-Restore` 恢复原始 `mtmft13.dll`）。
- 构建：`scripts/guest/mft/build-proxy.sh`（MSYS2 CLANGARM64，静态运行时）。

### 仍未闭环（如实记录）
- **麦克风（client→host 音频）**：upstream Sunshine 2026 版有 `mic_wasapi_t`，但需要 Moonlight VPlus
  客户端在 GUI 里开启麦克风；headless 下未实测。
- **双向剪贴板**：upstream Sunshine 的剪贴板是 client→host；host→client/双向需 Foundation Sunshine
  （其 ARM64 源码构建因 MinHook 缺失、无 mediafoundation 编码器而未采用）或额外桥接；本轮未验证通过。
- QGA 的 PowerShell 在 session 0，读不到用户会话剪贴板，后续剪贴板测试建议在 GUI 内做。
