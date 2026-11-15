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
