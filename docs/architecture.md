# 架构与设计取舍

**[English](architecture.en.md)**

```
                       宿主 Linux（ARM64）
  ┌──────────────────────────────────────────────────────────────┐
  │                                                              │
  │   qemu-system-aarch64（上游，无补丁）                        │
  │     -M virt -accel kvm -cpu host -m 8192 -smp 8              │
  │     AAVMF pflash  ──────────────┐                            │
  │     virtio-blk-pci  ──────────┐ │                            │
  │     virtio-gpu-pci  ────────┐ │ │                            │
  │     vfio-pci（mdev）──────┐ │ │ │                            │
  │     intel-hda             │ │ │ │                            │
  │   -display dbus ──────────┼─┼─┼─┼──► 私有 D-Bus 套接字       │
  │   -qmp unix:...sock ──────┼─┼─┼─┼──► QMP（输入、电源）       │
  └───────────────────────────┼─┼─┼─┼───────────────────────────┘
                              │ │ │ │
                              │ │ │ └──► M1000 vGPU（3D 加速）
                              │ │ └────► qcow2 系统盘
                              │ └──────► UEFI 变量
                              │
                ┌─────────────┴──────────────────┐
                │  winvm-viewer（X11）           │
                │   D-Bus 显示客户端             │
                │   QMP 键盘/鼠标                │
                │   剪贴板桥                     │
                │   分辨率跟随                   │
                └─────────────┬──────────────────┘
                              │ TCP 8790 / 8791
                              ▼
                    guest agent + 音频采集
```

## 为什么这么选

### 显示：RDP + RemoteFX（默认）

显示交给 Windows 自带的远程桌面栈：guest 侧由 GPU 的硬件编码器编 AVC444，宿主侧
硬件解码。选择它的理由是"需要的都由协议提供"——剪贴板、音频重定向、动态分辨率、
驱动器重定向、输入法都在协议里，不必在宿主上各写一套；同时任何支持 RemoteFX 的
RDP 客户端都能接，不绑定某一个客户端实现。

D-Bus 显示 + 自带 viewer 保留为兜底路径（`MODE=viewer`），用于 RDP 不可用的场合。
它把 scanout 缓冲**内联**在消息里，正好是 X11 `XPutImage` 想要的形状，viewer 可以直接
内存拷贝上屏——简单，但画面搬运全部落在宿主 CPU 上。

考虑过的替代方案：

* **`egl-headless`** —— 需要宿主有可用的 GL 上下文，给你的是一个纹理而不是窗口；
* **VNC** —— 能用，但多一层网络协议，且帧缓冲尺寸在连接时就固定了，分辨率跟随做不了；
* **SPICE** —— 要拉起整套服务端。

### 磁盘：`virtio-blk-pci`

配官方 `viostor` 驱动，是最无聊也最快、最长命的选项。

⚠️ **不要加 `disable-modern=on`**。现代模式被关掉后，Windows 只会读出几 MB 就卡死在启动阶段
（我们踩过，排查了很久）。

### GPU：标准 `vfio-pci`

摩尔线程 M1000 以**介质设备（mdev）**的形式暴露。`vfio-pci,sysfsdev=<uuid>` 就能把它挂上去，
和挂任何 mdev 一样；guest 里装官方 Windows 驱动，就得到一个正常的 D3D 适配器。

**不需要任何 QEMU 侧私有设备**。曾经有说法认为必须配套私有设备才能用，实测是不需要的——
guest 驱动自己就能工作。

⚠️ **不要删除 mdev**。`echo 1 > /sys/bus/mdev/devices/<uuid>/remove` 在某些 vGPU 驱动上
会直接崩掉宿主内核。启动器只创建、从不删除。

### 音频：`intel-hda` + `-audiodev pa`

HDA 模拟 + QEMU 原生 PulseAudio 后端，一行参数的事，Windows 自带驱动就能出声。

### UEFI：AAVMF

必须用 AAVMF（`qemu-efi-aarch64`），而且要用 **no-secboot** 那一份：

* `AAVMF_CODE.no-secboot.fd` 作为只读固件
* 一份 `AAVMF_VARS.fd` 的拷贝作为可写变量存储（每个 VM 一份，不要共用）

用某些私有固件是跑不起来的：它们依赖配套的显示设备才能出图。

## 启动流程

1. `winvm start` 启动 QEMU（daemonize），必要时创建 mdev。
2. AAVMF 运行 → Windows 启动 → 远程桌面服务就绪。
3. `winvm` 等 guest 就绪后拉起 RDP 客户端连接 `127.0.0.1:3389`，自动登录。
   兜底路径（`MODE=viewer`）则改为启动私有 D-Bus，再启动 viewer 连上总线和 QMP 套接字。
4. 兜底路径下，viewer 在收到第一帧前先画启动画面；AAVMF 起来后 `viogpudo`
   设置模式，QEMU 发出 `Scanout`（带新尺寸），viewer 调整窗口并开始按 `Update`
   脏矩形上屏。
5. guest agent 大约 60–100 秒后起来；此后剪贴板与分辨率由 RDP 协议（或兜底路径下
   的 agent）负责双向同步。
