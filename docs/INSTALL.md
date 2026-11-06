# 安装指南

把 Windows on ARM 虚拟机当作本地桌面使用：宿主机上的 RDP 客户端是用户**唯一**的交互界面，
虚拟机在后台运行，托盘控制面板负责状态显示与开关机。

```
┌────────────────────── 宿主机 (Linux/ARM64) ──────────────────────┐
│  winvm-tray (托盘)  ──►  winvm start/stop/show  ──►  QEMU         │
│                                                     │             │
│  FreeRDP 客户端 ◄──── RDP (AVC444, M1000 硬件编码) ──┘             │
└──────────────────────────────────────────────────────────────────┘
                              │ vfio-pci (M1000 vGPU)
┌──────────────────────── Windows guest ───────────────────────────┐
│  install-guest.ps1: 开 RDP + 帧率/编码策略 + 多会话 + 省电         │
└──────────────────────────────────────────────────────────────────┘
```

> **验证环境**：Ubuntu 24.04.4 LTS（内核 7.0.14，aarch64）、QEMU 8.2.2、
> FreeRDP 3.32.1、KDE Plasma 6 / Wayland。其它 ARM64 发行版理论上同样适用，
> 但下面的命令按 Debian/Ubuntu 写。

## 1. 宿主依赖

```sh
# Debian/Ubuntu (ARM64)
sudo apt install qemu-system-arm python3-gi gir1.2-ayatanaappindicator3-0.1 \
                 xdotool pulseaudio libusb-1.0-0
```

托盘依赖 **StatusNotifierItem**：

* **KDE Plasma** —— 原生支持，装好即用
* **GNOME** —— 需要扩展 `gnome-shell-extension-appindicator`
  （Ubuntu: `sudo apt install gnome-shell-extension-appindicator`，然后在
  「扩展」里启用 "AppIndicator and KStatusNotifierItem Support"）
* 其它桌面若没有托盘，退化为命令行：`sudo winvm show` / `winvm status`

## 2. 准备 Windows 虚拟机

### 2.1 安装 virtio 驱动

从官方项目下载 **virtio-win**：

* 稳定版 ISO：<https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/stable-virtio/virtio-win.iso>
* 项目主页（含各版本与校验和）：<https://github.com/virtio-win/virtio-win-pkg-scripts>
* 文档：<https://virtio-win.github.io/>

把 ISO 挂进虚拟机（或经共享目录拷进去），在 Windows 里安装以下驱动，
目录按 **`w11/ARM64`** 选（Windows 11 on ARM）：

| 驱动 | 作用 | 不装的后果 |
|---|---|---|
| `viostor` |  virtio-blk 磁盘 | 看不到硬盘 |
| `NetKVM` | virtio-net 网卡 | 没有网络 |
| `viogpudo` | GPU 显示（如用 virtio-gpu） | 显示异常 |
| `vioser` | virtio-serial | qemu-ga 不可用 |

安装方式：设备管理器 → 右键未知设备 → 更新驱动 → 浏览到 ISO 对应目录。

### 2.2 guest 配置（必做）

把 `scripts/install-guest.ps1`、`scripts/winvm-agent.ps1` 和 `guest/qemu-ga/`
整个目录一起拷进虚拟机（保持相对位置），管理员 PowerShell：

```powershell
powershell -ExecutionPolicy Bypass -File C:\install-guest.ps1
```

它会做六件事，每件都对应过实测：

0. **安装 qemu-ga**（`guest/qemu-ga/` 里随附的 Windows ARM64 二进制）
1. **开 RDP** 并放行防火墙
2. **解除 30fps 节流**：`DWMFRAMEINTERVAL=15` → 实测 32fps → 64.7fps
3. **启用 H.264/AVC 硬件编码**：`AVCHardwareEncodePreferred=1`
4. **允许同一用户多会话**：`fSingleSessionPerUser=0`（否则第二个连接 11 秒后被踢）
5. **关闭锁屏/睡眠/休眠**
6. （可选）部署会话助手 `winvm-agent.ps1` —— 仅用于额外的状态查询/调优，
   **不是必需的**：分辨率、DPI、剪贴板、音频全部由 RDP 协议通道完成

> 改注册表后脚本会**读回确认**。这一步不能省：
> 早前没读回，值没写进去却以为生效，导致把 32fps 误判成"驱动锁了刷新率"。

## 3. 安装宿主侧

```sh
sudo ./scripts/install-host.sh
```

它安装 `winvm` 与托盘、配置 XDG 自启、为托盘放开 `sudo winvm` 免密，
并检查是否已有可用的 FreeRDP。

### 3.1 FreeRDP：安装我们预编译的 deb（推荐）

发行版包的构建参数是：

```
WITH_GFX_H264=OFF  WITH_FFMPEG=OFF  WITH_OPENH264=OFF
```

**它根本不含 H.264 解码器**，`/gfx` 必然花屏。仓库里带了预编译包
`dist/freerdp3-winvm_3.32.1+winvm1_arm64.deb`（ARM64），装之前先卸掉系统自带的：

```sh
sudo apt remove --purge -y freerdp3-x11 libfreerdp3-3 libfreerdp-client3-3 libwinpr3-3
sudo apt install -y ./dist/freerdp3-winvm_3.32.1+winvm1_arm64.deb
```

这个包声明了 `Conflicts/Replaces/Provides`，装到 `/usr/lib/aarch64-linux-gnu`，
直接取代发行版的库。验证：

```sh
lib=$(ldd $(command -v xfreerdp) | awk '/libfreerdp3/{print $3; exit}')
strings "$lib" | grep -o 'WITH_GFX_H264=ON'     # 必须能打印出来
```

`winvm` 也是按这个能力选客户端的：找不到带 `WITH_GFX_H264=ON` 的客户端就报错，
不会静默用发行版那个花屏版本。

### 3.2 或者自己构建

```sh
sudo apt install cmake ninja-build build-essential libssl-dev libx11-dev \
  libxext-dev libxinerama-dev libxcursor-dev libxrandr-dev libxi-dev libxv-dev \
  libxfixes-dev libavcodec-dev libavutil-dev libswscale-dev libpulse-dev \
  libcups2-dev libpcsclite-dev libkrb5-dev libfuse3-dev libsystemd-dev \
  libcairo2-dev libjpeg-dev libusb-1.0-0-dev

git clone --depth 1 --branch 3.32.1 https://github.com/FreeRDP/FreeRDP
cd FreeRDP && mkdir build && cd build
cmake .. -GNinja -DCMAKE_BUILD_TYPE=Release \
  -DWITH_FFMPEG=ON -DWITH_SWSCALE=ON -DWITH_GFX_H264=ON -DWITH_VIDEO_FFMPEG=ON \
  -DWITH_X11=ON -DWITH_CLIENT=ON -DWITH_SERVER=OFF -DWITH_SHADOW=OFF \
  -DWITH_PULSE=ON -DWITH_ALSA=OFF -DWITH_CUPS=ON -DWITH_PCSC=ON \
  -DWITH_KRB5=ON -DWITH_FUSE=ON -DWITH_SYSTEMD=ON -DWITH_WAYLAND=OFF \
  -DWITH_CLIENT_SDL=OFF -DWITH_MANPAGES=OFF -DWITH_SAMPLE=OFF
ninja -j$(nproc)
cpack -G DEB                                  # 生成 freerdp-*.deb
sudo ./scripts/build-freerdp-deb.sh           # 打成可替代系统包的 freerdp3-winvm
```

> 注意：这里**没有源码 patch**。FreeRDP 上游本身支持 AVC444，
> 只是发行版把相关选项编译掉了。需要改的只有构建参数，
> `scripts/build-freerdp-deb.sh` 里做了重新打包（路径改到 multiarch、
> 补齐 Package/Version/Conflicts）。

## 4. 配置与启动

```sh
sudo tee /etc/winvm.conf >/dev/null <<'EOF'
MODE=rdp
START_RES=1920x1080
RDP_PORT=3389
RDP_BIND=127.0.0.1      # 默认只听本机；要对外改成 0.0.0.0
RDP_USER=Administrator
RDP_PASS=改成你的密码
EOF
sudo chmod 600 /etc/winvm.conf

sudo winvm start
```

`winvm start` 依次完成：启动 QEMU → 等 guest agent 就绪 → 拉起 RDP 客户端（自动登录）。

> **关于 `RDP_BIND`**：默认 `127.0.0.1`，只有本机连得上；改成 `0.0.0.0` 才会对局域网开放。

## 5. 托盘控制面板

登录桌面后自动出现（XDG autostart）。菜单：

| 项 | 行为 |
|---|---|
| 状态 | 如实显示：未运行 / 已暂停 / 运行中（画面未显示） / 运行中 / 异常 |
| 启动 Windows | 后台拉起虚拟机，不弹窗口；**未运行时只有这一项可用** |
| 停止 / 重启 Windows | 软关机 / 软重启（qemu-ga），失败退回 ACPI |
| 暂停 / 继续 | QMP 暂停与恢复，两者按当前状态互斥置灰 |
| 显示 Windows 窗口 | 只把画面拉回前台并自动登录，**不重启虚拟机**；虚拟机未运行时置灰 |

图标颜色：黑 = 没开机，绿 = 运行中，红 = 故障，琥珀 = 暂停中。

托盘由 `winvm-tray-run` 守护，进程退出会自动重启。

## 6. 验收自检

```sh
sudo winvm status                      # qemu / viewer / dbus
curl -s http://127.0.0.1:8790 -d '{"cmd":"status"}'   # guest 会话状态
```

guest 内应能看到：

```powershell
Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations' DWMFRAMEINTERVAL
# 15
```

浏览器打开 `http://<宿主>:8000/rafvid.html`（见 `bench/`）可测
视频+3D 负载下的真实帧率。

## 7. 分辨率 / DPI / 剪贴板 / 音频

这些**都是 RDP 标准能力**，由协议通道完成，guest 里不需要任何自定义服务：

| 能力 | 通道 | 用法 |
|---|---|---|
| 动态分辨率 | DisplayControl | 客户端 `/size:1920x1080`，会话桌面随之变化 |
| DPI 缩放 | DisplayControl | 客户端 `/scale:100/140/180` 或 `/dpi:` |
| 剪贴板 | cliprdr | 客户端 `+clipboard` |
| 音频 | rdpsnd | 客户端 `/sound` |
| 驱动器重定向 | rdpdr | 客户端 `/drive:share,/path` |

所以 `winvm show` 只是把客户端拉起来；分辨率、缩放、声音全部由 FreeRDP 参数决定。

## 8. qemu-ga

`winvm` 默认创建 `/tmp/winvm-qga.sock`（`org.qemu.guest_agent.0`）。
装上 qemu-ga 后：

```sh
sudo winvm stop      # 走 guest-shutdown(powerdown)，Windows 正常关机流程
sudo winvm reboot    # 走 guest-shutdown(reboot)
sudo winvm qga '{"execute":"guest-get-osinfo"}'    # 诊断透传
```

没装 qemu-ga 时 `winvm stop` 自动退回 QMP 的 ACPI 电源键（效果相当），
`winvm reboot` 会提示需要 qemu-ga。

**ARM64 没有官方 MSI**：virtio-win 0.1.302 只提供 i386 / x86_64
（见 ISO 的 `guest-agent/` 目录）。所以仓库随附了一份取自 MSYS2 clangarm64
的上游二进制 `guest/qemu-ga/`（QEMU 11.1.3 + glib/gettext/libiconv/pcre2），
`install-guest.ps1` 会自动安装并注册服务。

* 二进制 **原样提供、未作修改**，来源与版本见 `guest/qemu-ga/NOTICE.txt`
* 许可证全文（GPL-2.0 / LGPL-2.1 / BSD-3）见 `guest/qemu-ga/LICENSES/`
* x86_64 guest 也可直接用 virtio-win ISO 里的 `guest-agent/qemu-ga-x86_64.msi`
