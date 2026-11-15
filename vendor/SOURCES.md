# 第三方组件源码清单（SOURCES）

发布以**二进制安装**为主。只有以下情况把源码放进本仓库：

1. **体积小、上游不好找**：Sunshine、FreeRDP 的固定版本源码 tar（`vendor/src/`）；
2. **我们改过**：Moonlight V+ 的补丁（`vendor/patches/moonlight-qt/`）；
3. **guest 运行时缺的小文件**：QGA 的 GLib spawn helper（`guest/qemu-ga/`）。

QEMU、Qt 6、KDE、FFmpeg、SDL2、libplacebo 等**巨大的系统包不打包**，由用户用发行版安装。
GPL/LGPL 的源码提供见 [SOURCE-OFFER.md](SOURCE-OFFER.md)。

## 组件清单

| 组件 | 版本 / commit | 许可证 | 源码/获取方式 | 状态 |
|---|---|---|---|---|
| Moonlight V+ (qiin2333/moonlight-qt) | `6d176a8d9bcbb124d45563e689b593886d05d925` | GPL-3.0 | 上游 + `vendor/patches/moonlight-qt/0001,0002`；完整源树约 168 MB，按需 `--moonlight` 或书面提供 | 补丁在仓库；源码按需 |
| Sunshine (LizardByte/Sunshine) | `43b74505bc938e2a044d83b11a1ce13199a75753` | GPL-3.0 | `vendor/src/sunshine.tar.zst`（约 4.5 MB）；构建脚本 `scripts/guest/sunshine/` | **随仓库** |
| FreeRDP | `bf217a504e54cc719880c228e82353382cd7d4fa`（3.32.1+winvm1） | Apache-2.0 | `vendor/src/freerdp.tar.zst`（约 9.9 MB）；`dist/freerdp3-winvm_*.deb` | **随仓库** |
| QEMU | 8.2.2（Ubuntu `1:8.2.2+ds-0ubuntu1.18`） | GPL-2.0 | 发行版包；**未修改**；源码经 Ubuntu/上游 `qemu-8.2.2.tar.xz` | 系统包，不打包 |
| QEMU Guest Agent / virtio-win | 随 QEMU / virtio-win | GPL-2.0 / BSD-3-Clause 等 | `guest/qemu-ga/`（二进制 + LICENSES）；源码 QEMU `qga/`、virtio-win 仓库 | guest 二进制随仓库 |
| GLib（MSYS2 CLANGARM64） | 2.90.1-1 | LGPL-2.1-or-later | `guest/qemu-ga/gspawn-win64-helper{,-console}.exe`；上游 `glib-2.90.1.tar.xz` + MSYS2 PKGBUILD | helper 随仓库 |
| FFmpeg | 6.1.1-3ubuntu5 | LGPL-2.1-or-later（Ubuntu 构建） | 系统库；源码经发行版 | 系统包，用户安装 |
| SDL2 | 2.30.0+dfsg | zlib | 系统库；源码经发行版 | 系统包，用户安装 |
| libplacebo | 6.338.2 | LGPL-2.1-or-later | 系统库；源码经发行版 | 系统包，用户安装 |
| Qt 6（系统） | 6.4.2+dfsg | LGPL-3.0 / GPL-3.0 | 系统库；源码经发行版/Qt | 系统包，用户安装 |
| Qt 6（/opt/kde6） | 6.11.3 | LGPL-3.0 / GPL-3.0 | code.qt.io；KDE 各仓库 invent.kde.org | 预装依赖；源码 offer |
| KDE Plasma / Frameworks | `/opt/kde6`（6.x） | LGPL-2.1-or-later（各仓库不同） | invent.kde.org | 预装依赖；源码 offer |
| MT VPU 驱动 / MFT（`mtmft*.dll` 等） | 厂商版本 | **proprietary** | 无公开源码 | 见 [PROPRIETARY.md](PROPRIETARY.md) |
| Windows 11 ARM64 镜像 | 用户自备 | **proprietary** | 无 | 用户自备 |
| PES / MT 工具（`/opt/mt-pes`） | 厂商版本 | **proprietary** | 无 | 预装；不打包 |

## 本项目补丁

| 组件 | 补丁 | 说明 |
|---|---|---|
| Moonlight V+ | `vendor/patches/moonlight-qt/0001-qt6-overlay-opacity-notify.patch` | Qt6 overlay opacity 通知修复 |
| Moonlight V+ | `vendor/patches/moonlight-qt/0002-streamhub-stats.patch` | 每秒输出 `StreamHub stats`，供 StreamHub 显示码率/延迟/帧率 |
| FreeRDP | 无源码补丁 | 上游 commit + `scripts/build-freerdp-deb.sh` 打包 |
| Sunshine | 无源码补丁 | 上游 commit + `scripts/guest/sunshine/` 构建脚本 |
| QEMU | 无源码补丁 | 发行版包，不修改 |

## 版本核对

```bash
qemu-system-aarch64 --version
ffmpeg -version | head -1
dpkg -l | grep -E 'libsdl2|libplacebo|libqt6core|qt6-base'
/opt/kde6/bin/qmake6 -query QT_VERSION
git -C /opt/mtgpu-woa/src/moonlight-qt log -1 --format='%H %ci'
git -C /home/lf/build/sunshine-src log -1 --format='%H %ci'
git -C /home/lf/FreeRDP log -1 --format='%H %ci'
```
