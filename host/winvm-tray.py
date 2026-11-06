#!/usr/bin/env python3
"""winvm 托盘控制台。

宿主上的 RDP 客户端是用户唯一的交互界面，所以这个托盘要解决三件事：

1. 随时能看到虚拟机 / agent / RDP 客户端的状态；
2. 不用开终端就能启动、停止、重启；
3. 客户端掉了能自己拉回来（否则用户就"失联"了）。
"""

import os
import socket
import subprocess
import sys
import time

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("AppIndicator3", "0.1")
from gi.repository import AppIndicator3, GLib, Gtk  # noqa: E402

APP_ID = "winvm-tray"
WINVM = "/usr/local/bin/winvm"
AGENT_PORT = 8790
QMP_SOCK = "/tmp/qemudisplay.sock"
ICON_DIR = os.environ.get("WINVM_ICON_DIR", "/usr/local/lib/winvm/icons")

# 客户端掉了以后，最多这么快就尝试重连一次，避免崩溃循环里疯狂重启
RECONNECT_COOLDOWN = 15.0
POLL_SECONDS = 3


STATES = ("off", "run", "fault", "paused")


def ensure_icons():
    """兔头按状态染色：黑=没开机 绿=运行中 红=故障 半透明绿=暂停中。"""
    icons = {}
    for name in STATES:
        for size in (256, 128, 64):
            p = os.path.join(ICON_DIR, f"winvm-{name}-{size}.png")
            if os.path.exists(p):
                icons[name] = p
                break
    if len(icons) == len(STATES):
        return icons
    # 兜底：主题图标，保证托盘不空白
    return {n: "computer" for n in STATES}


def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True)


def pgrep(pattern):
    # 直接 pgrep -f <名字> 会匹配到外层 shell 自己的命令行（命令里就含这个字符串），
    # 于是"没开机"也会被判成在跑。用 [x] 写法让模式匹配不到自己。
    pat = "[" + pattern[0] + "]" + pattern[1:]
    r = sh(f"pgrep -f {pat!r}")
    return bool(r.stdout.strip())


def vm_running():
    # 优先看启动器写的 pidfile：没有匹配风险，也不受进程名长度限制
    try:
        with open("/tmp/winvm-qemu.pid") as f:
            pid = int(f.read().strip())
        with open(f"/proc/{pid}/comm") as f:
            if f.read().strip().startswith("qemu-system"):
                return True
    except (OSError, ValueError):
        pass
    return pgrep("qemu-system-aarch64")


def client_running():
    return pgrep("xfreerdp")


def qmp_status():
    """QMP socket 属于 root，托盘通过免密的 winvm state 读取。"""
    r = sh("sudo -n " + WINVM + " state")
    st = r.stdout.strip()
    return st if st and st != "unknown" else None


def agent_up(timeout=0.4):
    try:
        s = socket.create_connection(("127.0.0.1", AGENT_PORT), timeout=timeout)
        s.close()
        return True
    except OSError:
        return False


class Tray:
    def __init__(self):
        self.icons = ensure_icons()
        if os.environ.get("WINVM_NO_INDICATOR"):
            self.ind = None          # 只弹菜单、不注册托盘项
        else:
            self.ind = AppIndicator3.Indicator.new(
                APP_ID, self.icons["off"], AppIndicator3.IndicatorCategory.APPLICATION_STATUS
            )
            self.ind.set_status(AppIndicator3.IndicatorStatus.ACTIVE)
            self.ind.set_title("winvm")

        self.busy = False
        self.last_reconnect = 0.0
        self.last_state = None
        self.build_menu()

        GLib.timeout_add_seconds(POLL_SECONDS, self.tick)
        self.tick()

    def build_menu(self):
        m = Gtk.Menu()

        self.status_item = Gtk.MenuItem(label="正在检测…")
        self.status_item.set_sensitive(False)
        m.append(self.status_item)
        m.append(Gtk.SeparatorMenuItem())

        self.items = {}
        for key, label, action in (
            ("start", "启动 Windows", self.on_start),
            ("stop", "停止 Windows", self.on_stop),
            ("restart", "重启 Windows", self.on_restart),
        ):
            it = Gtk.MenuItem(label=label)
            it.connect("activate", action)
            m.append(it)
            self.items[key] = it

        m.append(Gtk.SeparatorMenuItem())

        it = Gtk.MenuItem(label="暂停")
        it.connect("activate", self.on_pause)
        m.append(it)
        self.items["pause"] = it

        it = Gtk.MenuItem(label="继续")
        it.connect("activate", self.on_resume)
        m.append(it)
        self.items["resume"] = it

        m.append(Gtk.SeparatorMenuItem())

        it = Gtk.MenuItem(label="显示 Windows 窗口")
        it.connect("activate", self.on_show)
        m.append(it)
        self.items["show"] = it

        it = Gtk.MenuItem(label="查看日志")
        it.connect("activate", self.on_logs)
        m.append(it)

        m.append(Gtk.SeparatorMenuItem())
        it = Gtk.MenuItem(label="退出")
        it.connect("activate", lambda *_: Gtk.main_quit())
        m.append(it)

        m.show_all()
        self.menu = m
        if self.ind is not None:
            self.ind.set_menu(m)

    # ---- 状态 ----

    def tick(self):
        vm = vm_running()
        cli = client_running()
        st = qmp_status() if vm else None

        if not vm:
            icon, text = "off", "Windows 未运行"
        elif st in ("paused", "suspended"):
            icon, text = "paused", "Windows 已暂停"
        elif st is None or st in ("crashed", "internal-error", "io-error", "guest-panicked"):
            icon, text = "fault", f"状态异常（{st or 'QMP 无响应'}）"
        elif st in ("shutdown", "prelaunch"):
            icon, text = "off", "Windows 已关闭"
        else:
            icon, text = "run", "Windows 运行中"
            if not cli:
                text = "Windows 在后台运行（画面未显示）"

        # Windows 没在后台跑时，"显示窗口"必须是灰的——用户得先点"启动"
        self.items["start"].set_sensitive(not vm)
        self.items["stop"].set_sensitive(vm)
        self.items["restart"].set_sensitive(vm)
        self.items["show"].set_sensitive(vm)
        paused = st in ("paused", "suspended")
        self.items["pause"].set_sensitive(vm and not paused)
        self.items["resume"].set_sensitive(vm and paused)

        if (icon, text) != self.last_state:
            if self.ind is not None:
                self.ind.set_icon_full(self.icons[icon], text)
            self.status_item.set_label(f"状态：{text}")
            self.last_state = (icon, text)
            if self.ind is not None:
                self.ind.set_title(f"winvm — {text}")
            with open("/tmp/winvm-tray.log", "a") as f:
                f.write(f"{time.strftime('%H:%M:%S')} vm={vm} qmp={st} client={cli} "
                        f"show_enabled={bool(vm)} -> {text}\n")

        # 不自动重连：虚拟机在后台继续跑，由用户点"显示 Windows 窗口"决定何时回到前台

        return True

    # ---- 动作 ----

    def notify(self, msg):
        sh(f"notify-send -a winvm {msg!r}")

    def spawn(self, cmd):
        """异步执行，避免卡住托盘。"""
        self.busy = True

        def run():
            subprocess.run(cmd, shell=True)
            self.busy = False

        import threading

        threading.Thread(target=run, daemon=True).start()

    def on_start(self, *_):
        self.spawn("sudo -n " + WINVM + " start")

    def on_stop(self, *_):
        self.spawn("sudo -n " + WINVM + " stop")

    def on_restart(self, *_):
        self.spawn(f"sudo -n {WINVM} stop && sudo -n {WINVM} start")

    def on_pause(self, *_):
        self.spawn("sudo -n " + WINVM + " pause")

    def on_resume(self, *_):
        self.spawn("sudo -n " + WINVM + " resume")

    def on_show(self, *_):
        # 只拉起画面客户端并自动登录，不动虚拟机
        self.spawn("sudo -n " + WINVM + " show")

    def on_logs(self, *_):
        # 没有桌面终端时退回通知
        for cmd in ("konsole -e tail -f /tmp/winvm-rdp.log",
                    "x-terminal-emulator -e tail -f /tmp/winvm-rdp.log",
                    "xterm -e tail -f /tmp/winvm-rdp.log"):
            if sh(f"command -v {cmd.split()[0]}").stdout.strip():
                sh(cmd + " &")
                return
        self.notify("日志：/tmp/winvm-rdp.log")


def main():
    t = Tray()
    # --show-menu：把菜单弹出在指定位置，便于截图/演示（不影响正常使用）
    if "--show-menu" in sys.argv:
        x, y = 120, 120
        try:
            i = sys.argv.index("--show-menu")
            x, y = int(sys.argv[i+1]), int(sys.argv[i+2])
        except (IndexError, ValueError):
            pass

        def popup():
            t.menu.popup(None, None, lambda *a: (x, y, True), None, 0,
                         Gtk.get_current_event_time())
            return False

        GLib.timeout_add(700, popup)
        GLib.timeout_add_seconds(int(os.environ.get("SHOW_MENU_SECONDS", "30")), Gtk.main_quit)
    Gtk.main()
    return 0


if __name__ == "__main__":
    sys.exit(main())
