# StreamHub — 客户端调研与设计

目标：为 mtgpu-woa 的 Moonlight/Sunshine 串流栈做一个**漂亮、高效、可改 UI** 的桌面客户端。
本文记录同类软件调研、取舍与最终设计。

## 1. 同类软件调研

| 产品 | 关键 UI/交互 | 值得借鉴 |
|---|---|---|
| Moonlight (Qt) | PC 发现网格 → 应用网格 → 串流；离线主机 40% 透明度、未知状态 spinner；QML + C++ 分层 | 两段式导航（主机→应用）、状态可视化 |
| Moonlight V+ | 串流内悬浮菜单、action sheet、主题选择器、紧凑行内菜单 | 悬浮/抽屉式控制、主题化 |
| Parsec | 极简、低干扰，overlay 按钮可关 | 主界面只保留必要信息 |
| Sunshine Web UI / Control Panel | 卡片式仪表盘（配对、安全、网络、显示） | 卡片式状态与操作入口 |
| GeForce NOW 改版 | 大卡片前置、去掉侧栏、突出搜索、简洁与可定制 | 大卡片、少层级 |
| Steam Link | Big Picture 主机式大磁贴、手柄可导航 | 大磁贴 + 键盘/手柄焦点 |
| Chiaki | 主机远程播放客户端，连接/串流一体 | 连接状态置于首屏 |

参考：
- Moonlight UI 架构：https://deepwiki.com/moonlight-stream/moonlight-qt/2-user-interface-system
- Moonlight 发现页：https://deepwiki.com/moonlight-stream/moonlight-qt/2.1-pc-discovery-view
- Moonlight Android UI：https://deepwiki.com/moonlight-stream/moonlight-android/4-user-interface
- Sunshine Control Panel 仪表盘：https://deepwiki.com/qiin2333/sunshine-control-panel/5.1-dashboard-interface
- GFN 改版（大卡片、去侧栏）：https://www.xda-developers.com/nvidia-geforce-major-redesign-windows-macos/
- Steam Link 大屏磁贴：https://mac.filehorse.com/download-steam-link/
- Moonlight V+ 悬浮菜单/主题：https://newreleases.io/project/github/qiin2333/moonlight-vplus/release/v12.10.8-game-menu-beta.1

## 2. 设计决策

- **状态优先**：顶栏 4 个状态胶囊（虚拟机 / Sunshine / 剪贴板 / 编码器），首屏即知是否可用。
- **英雄卡 + VPU 仪表**：主机信息、VPU 环形占用、编码器（MT VPU / 软件）、码率/分辨率/帧率一目了然。
- **应用大卡片**：Desktop / Steam Big Picture 等，悬停出现播放键，点击即串流；键盘方向键 + Enter 可操作（手柄友好）。
- **抽屉式设置**：不跳页，右侧滑出；分辨率/帧率/码率即时保存。
- **暗色 Aurora 玻璃风**：深色底 + 青/紫渐变光晕 + 细网格，动画克制（呼吸、卡片升起、环形过渡）。
- **无外部依赖**：纯 HTML/CSS/JS，字体用系统 Noto；离线可用，改样式只改 `style.css`。

## 3. 架构

```
┌─────────────── Chrome app 窗口（1280×860）───────────────┐
│  frontend: index.html + style.css + app.js               │
└───────────────▲──────────────────────────────────────────┘
                │ fetch /api/*
┌───────────────┴────────── FastAPI :8760 ─────────────────┐
│ status: qemu / 47989 / 47990 / mthreads-gmi / clip 8765  │
│ apps:   moonlight list 127.0.0.1                         │
│ stream: moonlight stream … --resolution --fps --bitrate  │
│ vm:     sudo -n winvm start|stop                         │
└──────────────────────────────────────────────────────────┘
```

- 串流画面仍由 **Moonlight V+** 承载（协议/解码/输入成熟），StreamHub 负责“选应用、定参数、看状态、一键连”。
- 选 FastAPI + SPA 的原因：宿主已有 Python/FastAPI/Chrome；无 Electron/QtWebEngine 依赖，迭代快，CSS 可深度定制。
- 安装：`scripts/install-streamhub.sh` → `/opt/mtgpu-woa/streamhub` + `~/.local/share/applications/streamhub.desktop`。

## 4. 后续可扩展

- 应用封面图（从 Sunshine apps.json / SteamGridDB 拉取）；
- 会话历史与最近使用；
- 手柄导航（SDL 事件或 Qt 游戏手柄）；
- 内嵌 Moonlight（Qt WebEngine / Moonlight 的 QML 组件复用）实现“无跳转”串流；
- 主题市场（CSS 变量主题切换）。

## 5. 实现与验证（2026-11-15）

代码：`apps/streamhub/`（`backend/app.py`、`frontend/{index.html,style.css,app.js,preview.html}`）、
安装脚本 `scripts/install-streamhub.sh`，安装到 `/opt/mtgpu-woa/streamhub`，桌面项
`~/.local/share/applications/streamhub.desktop`。

后端接口：`/api/status`（VM/Sunshine/剪贴板/VPU/编码器/设置，后台线程缓存，~65 ms）、
`/api/apps`、`/api/stream`（POST 启动 Moonlight）、`/api/stream/stop`、`/api/settings`、
`/api/vm`（sudo -n winvm start|stop）、`/api/vpu`。

实测（2026-11-15 18:50–18:55）：

- `/api/apps` → `Desktop`、`Steam Big Picture`；
- `POST /api/stream {"app":"Desktop"}` → pid 1377904，Moonlight 参数
  `--resolution 1920x1080 --fps 60 --bitrate 20000`；
- `/api/status` → `stream.running=true`、`vpu.encoder=25.3`、`encoder=mediafoundation`；
- `mthreads-gmi -q` → `Encoder: 25.4%`（MT VPU 实编码）。

UI 截图：`docs/images/streamhub-live.png`（实时数据，file:// 模式，CDP 抓取 1280×804）；
设计稿预览：`docs/images/streamhub-preview.png`。

### 环境注意与解决

从 SSH 会话启动的 Chrome/Chromium 在本机出现“HTTP 文档空白、`data:`/`file://` 正常”的
网络栈异常（连 `python -m http.server` 也空白），但 **`file://` 页面 fetch 本地 API 正常**。
因此客户端采用 **file:// 模式**：`streamhub.sh` 打开
`file:///opt/mtgpu-woa/streamhub/frontend/index.html`，页面资源本地加载，数据通过
`fetch('http://127.0.0.1:8760/api/*')`（后端已开 CORS）获取。
实测该模式下实时渲染正常：状态胶囊、VPU 25.4%、会话、2 个应用卡片均正确。
`http://127.0.0.1:8760/` 仍保留为普通浏览器访问方式（相对路径同源请求）。
