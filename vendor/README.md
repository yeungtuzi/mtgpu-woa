# 第三方源码与许可证（binary-first）

`mtgpu-woa` 的发布以**二进制安装**为主：不把 QEMU/Qt/KDE/FFmpeg 等上游大源码塞进仓库或
安装包。`vendor/` 负责**合规信息**：精确版本/commit、我们改过的补丁、源码获取方式，以及
GPL/LGPL 的**书面源码提供**（[SOURCE-OFFER.md](SOURCE-OFFER.md)）。

## 许可证策略（逐组件聚合）

本项目原创代码是 **Apache-2.0**（根目录 `LICENSE`）。分发的第三方组件**保留各自许可证**：

| 组件 | 许可证 | 分发方式 |
|---|---|---|
| Moonlight V+（含本项目补丁） | GPL-3.0 | 二进制 + 源码提供；补丁在 `patches/` |
| Sunshine（含构建脚本） | GPL-3.0 | guest 二进制；源码用上游 commit + 构建脚本 |
| FreeRDP（winvm deb） | Apache-2.0 | 预编译 deb；源码上游 commit + 打包脚本 |
| QEMU / QEMU Guest Agent | GPL-2.0 | **未修改**；使用发行版包，本项目不重分发 QEMU |
| Qt 6 / KDE Frameworks / GLib / libplacebo | LGPL-3.0 / LGPL-2.1-or-later | 系统/预装依赖；动态链接；源码经发行版/上游 |
| FFmpeg / SDL2 | LGPL-2.1-or-later / zlib | 系统依赖；源码经发行版/上游 |
| MT VPU 驱动/MFT、Windows 镜像、PES | proprietary | 不包含源码，见 [PROPRIETARY.md](PROPRIETARY.md) |

**为什么不做单一 GPLv3？** Apache-2.0 与 GPL-3.0 兼容，但 QEMU 是 GPL-2.0（不是
“or later”），不能整体重新许可为 GPL-3.0。正确做法是**聚合分发**：各组件按自己的许可证
分发；本项目不改变它们的许可证。所有开源组件都可与 Apache-2.0 原创代码一起聚合分发。

## 目录

```
vendor/
├── SOURCES.md                 # 组件/版本/许可证/源码来源/是否重分发
├── PROPRIETARY.md             # 专有组件与合规边界
├── SOURCE-OFFER.md            # GPL/LGPL 书面源码提供（默认合规路径）
├── LICENSES/                  # GPL-3.0/GPL-2.0/LGPL-3.0/LGPL-2.1/Apache-2.0/BSD 全文
├── patches/
│   └── moonlight-qt/          # 0001 overlay 修复、0002 StreamHub 统计
├── fetch-sources.sh           # 可选：按需拉取源码（不随默认安装执行）
└── src/                       # 可选生成物（.gitignore，默认不提交大文件）
```

## 源码获取（按需）

默认安装**不需要**下载源码。维护者/需要复核的用户可以按需拉取：

```bash
# 小源码（Sunshine/FreeRDP）已随仓库；默认只检查/补齐，不会重复下载
./vendor/fetch-sources.sh

# Moonlight V+ 完整源树 + 补丁（约 168 MB，体积大，默认不打包）
./vendor/fetch-sources.sh --moonlight

# 未修改的 QEMU（仅本地复核用；正常安装用发行版包）
./vendor/fetch-sources.sh --qemu

# 巨大的系统依赖源码（FFmpeg/SDL2/libplacebo/Qt6）
./vendor/fetch-sources.sh --system

# 查看/演练
./vendor/fetch-sources.sh --list
./vendor/fetch-sources.sh --dry-run
```

随仓库的 `vendor/src/sunshine.tar.zst`、`vendor/src/freerdp.tar.zst` 是**小体积/难找**
的固定版本源码；其余大包按需拉取或不提供，GPL/LGPL 合规走
[SOURCE-OFFER.md](SOURCE-OFFER.md) 的书面提供方式。

## 一键安装（二进制）

```bash
# 宿主
sudo ./scripts/install-host.sh
sudo ./scripts/install-streamhub-native.sh

# guest（Windows 11 ARM64 镜像需自备）
powershell -ExecutionPolicy Bypass -File scripts/install-guest.ps1
# Sunshine 源码构建见 scripts/guest/sunshine/ 与 scripts/build-sunshine-*
```

## 合规检查清单

- [ ] `SOURCES.md` 中重分发的组件都有版本/commit、许可证和源码获取方式。
- [ ] 我们修改过的代码（Moonlight 补丁、FreeRDP 打包、Sunshine 构建）有 patch/脚本可复现。
- [ ] `LICENSES/` 含所有组件的许可证全文。
- [ ] 发布说明包含 `SOURCE-OFFER.md`（或等价的书面源码提供）。
- [ ] QEMU 等未修改的系统包不重复打包，交给发行版源码渠道。
- [ ] 专有组件（MT VPU/MFT、Windows、PES）不进入 GPL/Apache 源码包。
