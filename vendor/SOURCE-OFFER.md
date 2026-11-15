# 源码提供（Source Offer）

本项目的发行版包含以下开源组件的**对象代码/二进制**：

- **GPL-3.0**：Moonlight V+（`qiin2333/moonlight-qt`，含本项目补丁）、Sunshine（`LizardByte/Sunshine`，guest 构建）。
- **GPL-2.0**：QEMU Guest Agent / virtio-win（随 guest 安装脚本使用）。
- **LGPL-3.0 / LGPL-2.1-or-later**：Qt 6、KDE Frameworks、GLib、libplacebo（若随发行版/运行环境提供）。
- **Apache-2.0**：FreeRDP（winvm deb）。
- **zlib**：SDL2（若随发行版提供）。

依据 GPLv3 §6、GPLv2 §3、LGPL 相应条款，我们向**任何收到上述对象代码的第三方**提供其
**完整对应源码**：

1. **网络方式**：<https://github.com/yeungtuzi/mtgpu-woa> 的 `vendor/` 目录包含
   `SOURCES.md`（精确版本/commit）、`patches/`（本项目修改）和 `fetch-sources.sh`
   （按需拉取完整上游源码）。Moonlight 的对应源码 = 上游 commit
   `6d176a8d9bcbb124d45563e689b593886d05d925` + `vendor/patches/moonlight-qt/` 的两个补丁；
   Sunshine 对应源码 = 上游 commit `43b74505bc938e2a044d83b11a1ce13199a75753` + 构建脚本；
   FreeRDP 对应源码 = 上游 commit `bf217a504e54cc719880c228e82353382cd7d4fa` + 打包脚本。
2. **书面/物理方式**：如网络方式不可用，可通过项目 issue 或维护者联系方式提出请求；
   我们可提供源码归档（介质/下载），收取不超过执行源码提供所需的合理成本。
3. **有效期**：自本发行版发布之日起**至少三年**。

## 不在本 offer 范围的组件

- **QEMU**：本项目**未修改** QEMU，使用 Ubuntu 8.2.2 发行版包（`1:8.2.2+ds-0ubuntu1.18`）；
  本项目不重分发 QEMU 二进制，其源码由 Ubuntu/上游提供（`https://download.qemu.org/qemu-8.2.2.tar.xz`）。
- **系统/预装依赖**：FFmpeg、SDL2、libplacebo、Qt 6、KDE Frameworks 等由发行版或
  `/opt/kde6` 运行环境提供；源码经对应发行版/上游（Qt: code.qt.io；KDE: invent.kde.org）。
- **专有组件**：MT VPU 驱动/MFT、Windows 11 ARM64 镜像、PES/MT 工具等没有可分发源码，
  见 [PROPRIETARY.md](PROPRIETARY.md)。它们不属于 GPL/LGPL 源码提供范围。

## 给下游再分发者

如果你再次分发本项目的二进制，请自行履行 GPL/LGPL 的源码提供义务（可保留本文件并提供
对应源码），并保留 `vendor/LICENSES/` 与 `vendor/SOURCES.md`。

> 本文件是项目维护者的合规声明，不构成法律意见；如有疑问请咨询法律顾问。
