# 更新记录

## 未发布

### 新增

- **KDE 欢迎画面主题 `aibook`**（`share/plasma/look-and-feel/org.aibook.splash`）：
  纯黑背景、兔子图标居中约占屏宽 1/4，边框四角颜色按绿→黄→红→蓝
  从灰色中依次点亮并循环；耳朵尖区域不受动画影响。
  安装：`./scripts/install-splash.sh`，随后在
  **系统设置 → 全局主题 → 欢迎画面** 选择 `aibook`。

## 1.0 (2026-11-06)

**主题：用 GPU 编解码器实现 RemoteFX 原生支持，性能相比原来的 viewer 大幅提升。**

这一版把显示通道整体换掉：画面不再由我们在宿主上逐帧搬运、渲染，而是交给
Windows 自带的远程桌面栈，由 GPU 的硬件编码器编码、宿主硬件解码。原来那条
D-Bus + 自带 viewer 的路径保留为兜底，但默认不再使用。

### 主要变化

- **显示走 RDP + RemoteFX**：guest 侧由 Windows 的远程桌面服务编码，走 AVC444
  （H.264 4:4:4）硬件编码路径；宿主用 FreeRDP 3 解码。编码器是 GPU 自带的，
  不额外占用 CPU 做软件编码。
- **解除默认帧率上限**：Windows 的远程桌面默认按 30 Hz 节流，这一版在 guest 安装
  脚本里调整了对应参数，高刷下可以跑满显示器的刷新率。
- **帧率**：1920×1080 下 WebGL 65.6 fps、1080p/30fps 视频跟满片源、桌面与 60 Hz
  刷新率一致。这是画面到屏幕的节奏，实际渲染由 vGPU 承担、能力远高于此。
  性能强于原厂预装的私有方案。
- **RDP 能做的都不再自己写**：剪贴板、音频重定向、动态分辨率、驱动器重定向、
  中文输入法、多显示器，全部用协议自带能力替换掉原先自研的实现。
- **控制面板**：托盘常驻，用项目图标按状态染色（黑=未开机、绿=运行中、红=故障、
  琥珀=暂停）。菜单提供启动/停止/重启/暂停/继续/"显示 Windows 窗口"/查看日志，
  没有运行时相应项自动置灰，不会自动把窗口拉回来打扰用户。
- **qemu-ga 集成**：软关机/软重启走 guest agent，硬电源操作才直接控制 QEMU；
  安装脚本可选装 qemu-ga 及其依赖。
- **预编译 FreeRDP**：发行版自带的 FreeRDP 编译时关掉了 GFX/H.264，解不了 AVC444。
  项目提供按同样上游源码构建的 `freerdp3-winvm` deb（仅改构建开关，未打源码补丁），
  安装脚本会在移除自带包后装上它。

### 修复

- 早前把 30 Hz 的帧率上限误判为 GPU 的显示模式限制，已在本文档中更正：
  那是远程桌面的默认节流，与 GPU 无关。

## 0.1.1 (2026-11-06)

在 KDE Plasma 6 / Wayland 桌面上把整套链路跑通，修掉了几个真空 bug。

### 修复

- **guest 没有键盘**：qemu 命令行里只有 `usb-tablet`，缺 `usb-kbd`，guest 只能动鼠标不能打字。
- **分辨率跟随失效**：guest agent 把显示器设备名写死成 `\\.\DISPLAY1`，而在多适配器
  （virtio-gpu + M1000 vGPU）环境下活动桌面可能是 `\\.\DISPLAY2`，导致 `displays`
  返回空数组、`setres` 返回 `rc=-99`。改为按 WinForms 的 `PrimaryScreen` 动态取设备名。
- **`EnumDisplayDevices` 对 DOD 适配器返回空**：显示枚举改用 `System.Windows.Forms.Screen`。
- **splash 被 TianoCore 标志覆盖**：`render()` 在 D-Bus 分支里无条件贴 guest 帧。
  现在固件把控制台交给 OS（`Disable`）之前一直保持自己的 splash，90 秒兜底。
- **KDE 下 viewer 起不来**：Xwayland 认证文件在 GNOME 叫 `.mutter-Xwaylandauth.*`，
  在 KDE 叫 `xauth_*`，启动脚本只找前者。两个都找。
- **窗口只有 1/4 屏、UI 显得极小**：guest 停在 virtio-gpu 默认分辨率。
  virtio-gpu 现在通过 EDID 宣告 `GUEST_W × GUEST_H`（默认 2560×1600），窗口跟着变。

### 新增

- **键鼠抓取**：点进窗口即 `XGrabPointer/XGrabKeyboard`，宿主光标被限制在窗口内，
  **Ctrl+Alt** 释放（VMware / Parallels 的手感）。
- **隐藏宿主光标**：guest 自己合成指针，本地 X11 光标换成 1×1 全透明，消除双指针与延迟。
- **缩放同步**：guest agent 新增 `getdpi` / `setdpi`（`PerMonitorSettings\…\DpiValue`，
  `DpiValue = 4 − 百分比/25`），启动器新增 `winvm dpi [percent]`，
  `winvm start` 结尾自动按桌面缩放比同步（`WINVM_AUTO_DPI=0` 可关）。**需在 guest 内注销重登生效。**
- **兔子图标**：`share/icons/` 下 6 档 PNG，`winvm.desktop` 的 `Icon=mtgpu-woa`，
  viewer 设置 `WM_CLASS=winvm` + `StartupWMClass`，菜单与任务栏都用同一图标。

## 0.1.0

首个公开版本：上游 QEMU + M1000 vGPU 直通 + D-Bus 显示协议 + 自带 X11 viewer。
