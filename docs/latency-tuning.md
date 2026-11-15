# 串流延迟调优（Moonlight V+ / Sunshine / MT VPU）

本文记录 2026-11 在本机（aibook + QEMU Windows 11 ARM64 + MT VPU）上对
Moonlight/Sunshine 串流延迟的实测与结论，供后续复现和调参使用。

## 结论（TL;DR）

1. **帧率是最大的杠杆。** MT VPU 的 H.264 MFT 有固定的帧级管线延迟：
   60fps 时 Sunshine 上报 `Encode ≈ 50ms`；1440×900@120（display device 实际切到
   166Hz）时 `Encode ≈ 21ms`。客户端 `Render 5.8ms / Decode 0.34ms`，网络 1ms。
2. **Sunshine 的配置键是 `minimum_fps_target`**（不是 `min_fps_target`）。
   默认 `0` 表示取客户端帧率的一半（60fps → ~30fps），静态画面会被压到 ~27fps，
   鼠标明显发滞。设为 `240` 后不再压帧。
3. **显示设备交给 Sunshine 程序化切换**：
   `dd_configuration_option=ensure_active`、`dd_resolution_option=auto`、
   `dd_refresh_rate_option=auto`。客户端请求分辨率/刷新率，Sunshine 在串流开始时
   自动切 guest 显示，无需去 Windows 设置里改（手动改会弹确认框且不串流）。
4. **码率不是延迟瓶颈。** 实测 10/20/40 Mbps 下 `Encode` 基本不变（~21ms）。
   localhost 没有带宽压力，推荐 30–50 Mbps 保画质；降低码率主要省带宽，
   不会增加编码延迟/负载。
5. **MT MFT 不暴露/不支持低延迟参数。** `CODECAPI_AVLowLatencyMode`、
   `AVEncCommonLowLatency`、`AVEncCommonRealTime`、`AVEncMPVDefaultBPictureCount`
   的 `IsSupported` 均为 0；`AVEncCommonRateControlMode` 的 `GetValue` 返回
   `E_NOTIMPL`。因此代理里设置这些属性不会改变延迟（已保留代码作为探测记录）。
6. **实际码率控制不是 CBR。** 40 Mbps 目标下静态桌面实测只有 ~13.6 Mbps；
   若 CBR 会填充到目标。FFmpeg `h264_mf` 的 `rate_control` 默认 `-1`（default），
   Sunshine 只传 `bit_rate`/`rc_max_rate`，由 MFT 内部决定，表现为 VBR 类
   （峰值受限/质量优先）；ABR/VBR 无法从 MFT 查询区分。
7. **官方 Sunshine 安装已删除**，只保留源码构建（计划任务 `sun-standalone`）。
   之前官方 `SunshineService` 会再拉起一个实例，导致配置被另一份覆盖
   （`minimum_fps_target` 看似不生效）。

## 实测数据

环境：guest 1440×900 / 1600×900，Sunshine 源码构建（h264_mf + 代理 MFT），
Moonlight V+ 客户端（VAAPI 解码），浮层为 Moonlight 的 performance overlay。

| 场景 | 分辨率/刷新 | 请求码率 | 实测带宽 | Encode | 客户端 FPS/Rd | VPU Encoder |
|---|---|---|---|---|---|---|
| 60fps 默认 | 1600×900@60 | 20 Mbps | 6.9–10.5 Mbps | ~50 ms | 60/60 | ~18% |
| 120fps 低延迟 | 1440×900@166 | 20 Mbps | 13.8 Mbps | 21.6 ms | 166/163 | 48% |
| 120fps 低延迟 | 1440×900@168 | 40 Mbps | 13.6 Mbps | 21.2 ms | 167/166 | 47% |
| 省电 | 1280×720@60 | 20 Mbps | — | — | — | 33% |

> 带宽是 Moonlight 浮层的实测值，静态桌面远低于目标码率，说明不是 CBR。

## 推荐配置

StreamHub 三种模式（`apps/streamhub/backend/app.py` 的 `MODE_PRESETS`）：

| 模式 | 分辨率 | 帧率 | 码率 | 说明 |
|---|---|---|---|---|
| 省电 `powersave` | 1280×720 | 60 | 20 Mbps | VPU/功耗最低，延迟约 50ms |
| 均衡 `balanced` | 1600×900 | 60 | 30 Mbps | 画质优先，延迟约 50ms |
| 低延迟 `low_latency` | 1440×900 | 120 | 40 Mbps | Encode ~21ms，VPU ~48%，功耗最高 |

Sunshine（`C:\mtgpu-woa\sunshine-built\config\sunshine.conf`）：

```ini
encoder = mediafoundation
min_log_level = info
dd_configuration_option = ensure_active
dd_resolution_option = auto
dd_refresh_rate_option = auto
minimum_fps_target = 240
```

Moonlight 侧：`vsync=false`、`framepacing=false`（`~/.config/Moonlight Game Streaming Project/Moonlight.conf`），
启动参数 `--display-mode windowed|fullscreen --ignore-aspect-ratio`。
运行中可用 `Ctrl+Alt+Shift+F` 切换全屏。

## MT MFT / 代理说明

- MT MFT 是 async MFT（`MF_TRANSFORM_ASYNC=1`，`OutputDelay: 1`），FFmpeg `h264_mf`
  的事件循环不能直接驱动；`scripts/guest/mft/mtmf_sync_proxy.cpp` 提供 async facade，
  合成 `NeedInput/HaveOutput` 事件、补 `CleanPoint`（IDR）、首帧强制 GOP=1 后恢复 GOP=60。
- 代理已尝试设置低延迟/B 帧属性并查询 rate control mode；实测 MFT 全部
  `IsSupported=0` / `GetValue=E_NOTIMPL`，不会改变行为。
- MFT 的管线延迟按“帧”计，因此**提高帧率**（120/165fps）才能把 ms 延迟降下来；
  分辨率/码率影响很小。

## QGA guest-exec 修复（重要）

Windows guest 的 QEMU Guest Agent 缺少 GLib 的 spawn helper，
`guest-exec` 报 `Failed to execute helper program (No such file or directory)`。
把 MSYS2 CLANGARM64 的 helper 放进 QGA 安装目录即可：

```bash
# 宿主（aibook）上下载并解包 MSYS2 glib2（ARM64）
mkdir -p ~/build/glib2 && cd ~/build/glib2
curl -LO https://repo.msys2.org/mingw/clangarm64/mingw-w64-clang-aarch64-glib2-2.90.1-1-any.pkg.tar.zst
tar --zstd -xf mingw-w64-clang-aarch64-glib2-2.90.1-1-any.pkg.tar.zst
B=clangarm64/bin
# 通过 QGA 文件写入 guest（qga_file.py 见宿主 /home/lf/qga_file.py）
sudo python3 ~/qga_file.py write 'C:\Program Files\Qemu-ga\gspawn-win64-helper.exe' < $B/gspawn-win64-helper.exe
sudo python3 ~/qga_file.py write 'C:\Program Files\Qemu-ga\gspawn-win64-helper-console.exe' < $B/gspawn-win64-helper-console.exe
# 验证
sudo python3 ~/qga_exec.py 'C:\Windows\System32\cmd.exe' /c 'echo hello-qga'
```

修好后可以用 QGA 直接跑 guest 命令（改配置、查显示模式、重启任务等），
不再依赖 guest SSH。

## Parsec 的取舍（为什么它显得“更跟手”）

- **客户端本地光标（client-side cursor）**：Parsec 专利
  [US10951890](https://patents.google.com/patent/US10951890B2/en) 明确写了在客户端
  本地模拟鼠标、显示 “local mouse”，让输入看起来低延迟；Moonlight/Sunshine 把光标
  画在视频里，光标延迟 = 整条编码/网络/解码/显示链路。社区也提过给
  Sunshine/Moonlight 加客户端光标（[LizardByte #978](https://github.com/orgs/LizardByte/discussions/978)）。
- **UDP 传输**：Parsec 媒体走基于 UDP 的 BUD 协议（TCP 只用于连接建立），
  丢包时用重传/FEC 换低延迟；Moonlight/Sunshine 视频走 TCP，丢包会队头阻塞。
  localhost 下这点无影响。
- **激进丢帧/自适应**：Parsec 以延迟为第一目标，拥塞/高负载时丢帧、降码率/分辨率；
  Moonlight 的 pacer 也会丢帧（`pacer.cpp` 的 `frameDropTarget`），但默认可关闭
  frame pacing 以换最低延迟。我们当前 `framepacing=false`。
- **硬件零拷贝 + 无 B 帧 + 低 VBV**：Parsec/游戏串流普遍这么做；我们这边
  `max_b_frames=0` 已由 Sunshine 设置，但 MT MFT 的固定管线延迟无法通过参数消除。
- 代价：Parsec 牺牲画质/带宽（低码率、丢帧、客户端光标可能与 host 光标形状/位置
  不同步）；Moonlight/Sunshine 更偏画质与确定性。若要在本方案进一步逼近，
  最有效的是**客户端光标**，但需要协议侧支持，暂未实现。

## 复现命令

```bash
# guest：重启 Sunshine（源码构建）
sudo python3 /home/lf/qga_exec.py 'C:\Windows\System32\schtasks.exe' /run /tn sun-standalone

# 宿主：切模式并重启串流
curl -s -X POST http://127.0.0.1:8760/api/stream/stop -H 'Content-Type: application/json' -d '{}'
curl -s -X POST http://127.0.0.1:8760/api/stream -H 'Content-Type: application/json' -d '{"mode":"low_latency"}'

# guest：查看 Sunshine 的 fps/刷新率日志
curl -sk -u admin:Winvm!2026 https://127.0.0.1:47990/api/logs | grep -E 'Display refresh|Requested frame|Minimum FPS'
```
