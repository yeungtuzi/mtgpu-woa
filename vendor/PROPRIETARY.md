# 专有组件（无源码可分发）

以下组件**不属于开源第三方源码包**，不能随 Apache-2.0/GPL 源码一起重新许可或公开源码。
本项目只提供集成脚本/包装层；使用这些组件需要用户自行从合法渠道获得并遵守其许可。

## 1. MT VPU 驱动 / 硬件编码 MFT

- 文件示例：guest `C:\mtgpu-woa\mtmft*.dll`（如 `mtmft13.dll`）、`/opt/m1000-mtc-toolkit`、MT vGPU 驱动/固件。
- 作用：MediaTek M1000 vGPU 的 H.264 硬件编码 MFT，注册在 NVIDIA H.264 Encoder MFT CLSID
  `{60F44560-5A20-4857-BFEF-D29773CB8040}` 上。
- 许可：厂商专有；**没有公开源码**，不得反编译/再分发（除非厂商许可）。
- 本项目的 `scripts/guest/mft/mtmf_sync_proxy.cpp` 是 Apache-2.0 的**包装层**，只调用已安装的
  专有 MFT；没有它时硬件编码不可用（可退回软件编码，性能/延迟不同）。
- 获取：随设备/厂商镜像提供；本项目不附带。

## 2. Windows 11 ARM64 镜像与授权

- Windows 镜像、驱动、激活授权属于 Microsoft；本项目**不分发** Windows 镜像。
- 用户需自备 Windows 11 ARM64 ISO/许可，按 `docs/INSTALL.md` 安装到 QEMU。
- 镜像内的 Microsoft 组件（含 DirectX、Media Foundation 等）受 Microsoft 许可约束。

## 3. MT / 厂商工具

- `/opt/mt-pes`（PES 控制中心）、`mtvmm`、`mtgpu-chromium` 等预装工具为厂商/第三方专有软件。
- 本项目只在需要时调用/集成，不打包其源码，也不对其再分发。

## 4. 其他可能的专有依赖

- GPU 厂商驱动（NVIDIA/AMD/Intel/MediaTek）及其固件。
- 任何未在 [SOURCES.md](SOURCES.md) 列出的二进制库。

## 合规边界

- 开源源码包（`vendor/src` / `dist/mtgpu-woa-sources-*.tar.zst`）**只覆盖** SOURCES.md 中标记为
  core/system 的开源组件。
- 专有组件不进入 GPL/Apache 源码包；它们与开源部分构成“聚合分发”，各自许可证独立。
- 若再分发本项目的发行版，请同时保留本文件和 [SOURCES.md](SOURCES.md)、`vendor/LICENSES/`，
  并确保用户能从合法渠道获得上述专有组件。
