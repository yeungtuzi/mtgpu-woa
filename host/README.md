# 托盘控制面板

`winvm-tray.py` 是宿主侧的托盘应用，显示 Windows 是否还在后台运行，
并提供启动 / 停止 / 重启 / 「显示 Windows 窗口」。

* 依赖 GTK3 + AyatanaAppIndicator3（KDE 原生支持；GNOME 需要
  `gnome-shell-extension-appindicator`）
* `winvm-tray-run.sh` 负责守护：进程退出会自动重启
* 状态来源：QEMU 进程、FreeRDP 进程（guest 助手是可选的，不参与判定）
* 「显示 Windows 窗口」只拉起客户端并自动登录，不重启虚拟机；
  虚拟机未运行时该项置灰
