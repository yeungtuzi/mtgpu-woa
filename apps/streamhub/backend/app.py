"""StreamHub backend — a small control plane for the mtgpu-woa Moonlight/Sunshine stack.

Runs locally as the desktop user; the frontend is served from ../frontend.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import signal
import socket
import subprocess
import threading
import time
from pathlib import Path
from typing import Any

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

BASE = Path(__file__).resolve().parent.parent
FRONTEND = BASE / "frontend"
CONFIG_DIR = Path.home() / ".config" / "streamhub"
SETTINGS_FILE = CONFIG_DIR / "settings.json"

MOONLIGHT = "/opt/mtgpu-woa/moonlight/bin/moonlight"
STATS_LOG = Path("/opt/mtgpu-woa/moonlight/streamhub-stats.log")
GUEST_FPS_SCRIPT = "/opt/mtgpu-woa/streamhub/set-guest-fps.sh"
GUEST_DISPLAY_SCRIPT = "/opt/mtgpu-woa/streamhub/set-guest-display.sh"
WINVM = "/usr/local/bin/winvm"
CLIP_BRIDGE = "http://127.0.0.1:8765/clip"
PORT = 8760

DEFAULT_SETTINGS = {
    "width": 1600,
    "height": 900,
    "fps": 60,
    "bitrate": 25000,
    "app": "Desktop",
    "display_mode": "windowed",
    "ignore_aspect": True,
    "mode": "balanced",
}

MODE_PRESETS = {
    # 三档明确对应帧率：节能 30 / 均衡 60 / 低延迟 120。
    "powersave": {"width": 1280, "height": 720, "fps": 30, "bitrate": 15000},
    "balanced": {"width": 1600, "height": 900, "fps": 60, "bitrate": 25000},
    "low_latency": {"width": 1440, "height": 900, "fps": 120, "bitrate": 30000},
}


def load_settings() -> dict[str, Any]:
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    if SETTINGS_FILE.exists():
        try:
            data = json.loads(SETTINGS_FILE.read_text())
            merged = {**DEFAULT_SETTINGS, **data}
            # Migrate settings written before the mode selector existed.
            if "mode" not in data:
                merged["mode"] = (
                    "low_latency" if merged.get("fps", 60) >= 120
                    else "powersave" if merged.get("width", 0) <= 1280
                    else "balanced"
                )
            return merged
        except Exception:
            pass
    return dict(DEFAULT_SETTINGS)


def save_settings(data: dict[str, Any]) -> dict[str, Any]:
    merged = {**load_settings(), **data}
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    SETTINGS_FILE.write_text(json.dumps(merged, indent=2))
    return merged


def display_env() -> dict[str, str]:
    env = dict(os.environ)
    env.setdefault("DISPLAY", ":0")
    if "XAUTHORITY" not in env:
        import glob

        candidates = glob.glob(f"/run/user/{os.getuid()}/xauth_*")
        if candidates:
            env["XAUTHORITY"] = candidates[0]
    return env


def run(cmd: list[str], timeout: float = 15, env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=timeout,
        env=env or display_env(),
    )


def tcp_open(port: int, host: str = "127.0.0.1", timeout: float = 0.4) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def qemu_pid() -> int | None:
    try:
        out = run(["pgrep", "-f", "qemu-system-aarch64.*woa-hist"], timeout=3).stdout
        pids = [int(x) for x in out.split() if x.strip()]
        return pids[0] if pids else None
    except Exception:
        return None


def _vpu_stats_live() -> dict[str, Any]:
    try:
        out = run(["mthreads-gmi", "-q"], timeout=5).stdout
    except Exception:
        return {"encoder": 0.0, "decoder": 0.0, "temp": None}
    enc = re.search(r"Encoder:\s*([\d.]+)%", out)
    dec = re.search(r"Decoder:\s*([\d.]+)%", out)
    temp = re.search(r"Temperature:\s*([\d.]+)C", out)
    return {
        "encoder": float(enc.group(1)) if enc else 0.0,
        "decoder": float(dec.group(1)) if dec else 0.0,
        "temp": float(temp.group(1)) if temp else None,
    }


_apps_cache: dict[str, Any] = {"ts": 0.0, "apps": []}

# Background caches: mthreads-gmi / curl are slow (seconds); the UI polls every
# 2 s, so answering from a warm cache keeps the page's network idle.
_cache: dict[str, Any] = {
    "vpu": {"encoder": 0.0, "decoder": 0.0, "temp": None},
    "clipboard": False,
    "ts": 0.0,
}


def _poller() -> None:
    while True:
        try:
            _cache["vpu"] = _vpu_stats_live()
        except Exception:
            pass
        try:
            _cache["clipboard"] = (
                subprocess.run(
                    ["curl", "-s", "--max-time", "1", CLIP_BRIDGE],
                    capture_output=True,
                    timeout=3,
                ).returncode == 0
            )
        except Exception:
            _cache["clipboard"] = False
        _cache["ts"] = time.time()
        time.sleep(1.5)


threading.Thread(target=_poller, daemon=True).start()


def list_apps() -> list[dict[str, str]]:
    now = time.time()
    if now - _apps_cache["ts"] < 30 and _apps_cache["apps"]:
        return _apps_cache["apps"]
    apps: list[dict[str, str]] = []
    try:
        env = display_env()
        proc = run([MOONLIGHT, "list", "127.0.0.1"], timeout=20, env=env)
        for line in (proc.stdout or "").splitlines():
            line = line.strip()
            if not line or re.match(r"^\d\d:\d\d:\d\d", line):
                continue
            if line.startswith(("Qt ", "SDL ", "00:")):
                continue
            apps.append({"name": line})
    except Exception:
        apps = []
    if not apps:
        apps = [{"name": "Desktop"}, {"name": "Steam Big Picture"}]
    _apps_cache["ts"] = now
    _apps_cache["apps"] = apps
    return apps


class StreamRequest(BaseModel):
    app: str | None = None
    width: int | None = None
    height: int | None = None
    fps: int | None = None
    bitrate: int | None = None
    display_mode: str | None = None
    ignore_aspect: bool | None = None
    mode: str | None = None


class SettingsRequest(BaseModel):
    width: int | None = None
    height: int | None = None
    fps: int | None = None
    bitrate: int | None = None
    app: str | None = None
    display_mode: str | None = None
    ignore_aspect: bool | None = None
    mode: str | None = None


class FullscreenRequest(BaseModel):
    fullscreen: bool


class VmRequest(BaseModel):
    action: str


_stream_proc: subprocess.Popen | None = None
_stream_info: dict[str, Any] = {"app": None, "pid": None, "started": None}


def stream_pids() -> list[int]:
    """Real Moonlight stream processes (argv[0]=.../moonlight, argv[1]=stream).

    Using pgrep -f here would also match shells whose command line mentions the
    pattern, which made the hub think a stream was always running.
    """
    pids: list[int] = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            parts = (entry / "cmdline").read_bytes().split(b"\0")
        except Exception:
            continue
        if len(parts) >= 2 and parts[0].endswith(b"/moonlight") and parts[1] == b"stream":
            pids.append(int(entry.name))
    return pids


def stream_running() -> bool:
    global _stream_proc
    if _stream_proc is not None and _stream_proc.poll() is None:
        return True
    return bool(stream_pids())


app = FastAPI(title="StreamHub")
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.get("/api/status")
def status() -> dict[str, Any]:
    vm = qemu_pid()
    vpu = _cache["vpu"]
    clip_ok = bool(_cache["clipboard"])
    return {
        "vm": {"running": vm is not None, "pid": vm},
        "sunshine": {"reachable": tcp_open(47989), "webui": tcp_open(47990)},
        "stream": {"running": stream_running(), "app": _stream_info["app"]},
        "vpu": vpu,
        "clipboard": {"ok": clip_ok},
        "encoder": "mediafoundation" if (stream_running() and vpu["encoder"] > 0.5) else "idle",
        "settings": load_settings(),
    }


@app.get("/api/apps")
def apps() -> list[dict[str, str]]:
    return list_apps()


@app.post("/api/stream")
def start_stream(req: StreamRequest) -> JSONResponse:
    global _stream_proc, _stream_info
    settings = load_settings()
    app_name = req.app or settings["app"]
    mode = req.mode or settings.get("mode", "balanced")
    preset = MODE_PRESETS.get(mode, {})
    width = req.width or preset.get("width") or settings["width"]
    height = req.height or preset.get("height") or settings["height"]
    fps = req.fps or preset.get("fps") or settings["fps"]
    bitrate = req.bitrate or preset.get("bitrate") or settings["bitrate"]
    # Keep the guest Windows display mode and capture rate in sync with the
    # requested stream. Manual mode is deliberate: the MT virtual display does
    # not reliably honor client-driven mode changes. The helper only restarts
    # Sunshine when the target actually changes.
    if stream_running():
        return JSONResponse({"ok": False, "error": "已有串流在运行，请先停止"}, status_code=409)
    try:
        guest_display = subprocess.run(
            [GUEST_DISPLAY_SCRIPT, f"{width}x{height}", str(fps)],
            capture_output=True, text=True, timeout=60,
        )
    except Exception as exc:
        return JSONResponse(
            {"ok": False, "error": f"设置 Windows 分辨率失败: {exc}"},
            status_code=500,
        )
    if guest_display.returncode != 0:
        detail = (guest_display.stdout + guest_display.stderr)[-500:].strip()
        return JSONResponse(
            {"ok": False, "error": "设置 Windows 分辨率失败: " + detail},
            status_code=500,
        )
    display_mode = req.display_mode or settings.get("display_mode", "windowed")
    ignore_aspect = req.ignore_aspect if req.ignore_aspect is not None else settings.get("ignore_aspect", False)
    cmd = [
        MOONLIGHT,
        "stream",
        "127.0.0.1",
        app_name,
        "--resolution",
        f"{width}x{height}",
        "--fps",
        str(fps),
        "--bitrate",
        str(bitrate),
        "--display-mode",
        display_mode,
    ]
    if ignore_aspect:
        cmd.append("--ignore-aspect-ratio")
    try:
        # Moonlight writes its per-second "StreamHub stats" block to stderr.
        stats_fh = open(STATS_LOG, "wb", buffering=0)
        try:
            _stream_proc = subprocess.Popen(
                cmd,
                stdout=subprocess.DEVNULL,
                stderr=stats_fh,
                start_new_session=True,
                env=display_env(),
            )
        finally:
            stats_fh.close()
    except Exception as exc:  # pragma: no cover
        return JSONResponse({"ok": False, "error": str(exc)}, status_code=500)
    _stream_info = {"app": app_name, "pid": _stream_proc.pid, "started": time.time()}
    save_settings({"app": app_name, "width": width, "height": height, "fps": fps,
                   "bitrate": bitrate, "display_mode": display_mode,
                   "ignore_aspect": ignore_aspect, "mode": mode})
    return JSONResponse({"ok": True, "pid": _stream_proc.pid, "app": app_name})


@app.post("/api/stream/stop")
def stop_stream() -> dict[str, Any]:
    global _stream_proc
    stopped = False
    if _stream_proc is not None and _stream_proc.poll() is None:
        try:
            os.killpg(os.getpgid(_stream_proc.pid), signal.SIGTERM)
            stopped = True
        except Exception:
            pass
    for pid in stream_pids():
        try:
            os.kill(pid, signal.SIGTERM)
            stopped = True
        except Exception:
            pass
    _stream_proc = None
    return {"ok": True, "stopped": stopped}


@app.post("/api/stream/fullscreen")
def stream_fullscreen(req: FullscreenRequest) -> Any:
    """Toggle EWMH fullscreen on the running Moonlight window (no reconnect)."""
    env = display_env()
    try:
        out = subprocess.check_output(["wmctrl", "-l"], env=env, text=True, timeout=3)
    except Exception as exc:
        return JSONResponse({"ok": False, "error": f"wmctrl 失败: {exc}"}, status_code=500)
    wid = None
    for line in out.splitlines():
        if "qemu-woa - Moonlight" in line or "qemu-woa" in line:
            wid = line.split()[0]
            break
    if wid is None:
        for line in out.splitlines():
            if "Moonlight" in line and "V+ for PC" not in line and "PES" not in line:
                wid = line.split()[0]
                break
    if wid is None:
        return JSONResponse({"ok": False, "error": "串流窗口未找到"}, status_code=409)
    action = "add" if req.fullscreen else "remove"
    try:
        subprocess.run(["wmctrl", "-i", "-r", wid, "-b", f"{action},fullscreen"],
                       env=env, timeout=3, check=False)
    except Exception as exc:
        return JSONResponse({"ok": False, "error": str(exc)}, status_code=500)
    return {"ok": True, "fullscreen": req.fullscreen}


@app.get("/api/stream/stats")
def stream_stats() -> dict[str, Any]:
    """Parse the latest Moonlight stats block emitted once per second."""
    running = stream_running()
    if not running:
        return {"ok": False, "running": False}
    try:
        text = STATS_LOG.read_text(errors="replace")[-20000:]
    except Exception:
        return {"ok": False, "running": True}
    idx = text.rfind("StreamHub stats")
    if idx >= 0:
        text = text[idx:]

    m_fps = re.search(r"FPS\s+([\d.]+)", text)
    m_rx = re.search(r"([\d.]+)\s*(?:\{\d+\})?\s*Rx", text)
    m_de = re.search(r"([\d.]+)\s*(?:\{\d+\})?\s*De", text)
    m_rd = re.search(r"([\d.]+)\s*(?:\{\d+\})?\s*Rd", text)
    m_bw = re.search(r"Bandwidth\s+\**([\d.]+)\**\s*(?:\{16\})?\s*Mbps", text)
    # Parse these independently: Moonlight omits Encode entirely when Sunshine
    # has not reported host-processing latency (framesWithHostProcessingLatency=0).
    m_render = re.search(r"Render\s+\**([\d.]+)\**ms", text)
    m_decode = re.search(r"Decode\s+\**([\d.]+)\**ms", text)
    m_encode = re.search(r"Encode\s+\**([\d.]+)\**ms", text)
    m_net = re.search(r"Network\s+\**(\d+)\**\s*±\s*(\d+)ms", text)
    m_loss = re.search(r"Loss\s+([\d.]+)%", text)

    def f(m, i):
        try:
            return float(m.group(i)) if m else None
        except Exception:
            return None

    fps = f(m_fps, 1); rx = f(m_rx, 1); de = f(m_de, 1); rd = f(m_rd, 1)
    bw = f(m_bw, 1)
    render = f(m_render, 1); decode = f(m_decode, 1); encode = f(m_encode, 1)
    net = f(m_net, 1)
    loss = f(m_loss, 1)
    # End-to-end latency is only meaningful when every component exists.
    # Otherwise show "--"; never present network-only 1ms as total latency.
    latency = None
    if all(v is not None for v in (encode, render, decode, net)):
        latency = round(encode + render + decode + net, 1)
    missing = [name for name, v in (("encode", encode), ("render", render),
                                    ("decode", decode), ("network", net))
               if v is None]
    ok = any(v is not None for v in (fps, bw, render, decode, encode))
    return {
        "ok": ok, "running": True,
        "fps": fps, "rx": rx, "de": de, "rd": rd,
        "bandwidth": bw, "render": render, "decode": decode,
        "encode": encode, "network": net, "loss": loss,
        "latency": latency, "missing": missing,
    }


@app.get("/api/settings")
def get_settings() -> dict[str, Any]:
    return load_settings()


@app.put("/api/settings")
def put_settings(req: SettingsRequest) -> dict[str, Any]:
    data = {k: v for k, v in req.model_dump().items() if v is not None}
    # A mode-only update applies that mode's preset; explicit fields win.
    if data.get("mode") and not any(k in data for k in ("width", "height", "fps", "bitrate")):
        data.update(MODE_PRESETS.get(data["mode"], {}))
    return save_settings(data)


@app.post("/api/vm")
def vm_control(req: VmRequest) -> dict[str, Any]:
    if req.action not in ("start", "stop"):
        return {"ok": False, "error": "action must be start or stop"}
    try:
        proc = subprocess.run(
            ["sudo", "-n", WINVM, req.action],
            capture_output=True,
            text=True,
            timeout=90,
        )
        return {"ok": proc.returncode == 0, "output": (proc.stdout + proc.stderr)[-2000:]}
    except Exception as exc:
        return {"ok": False, "error": str(exc)}


@app.get("/api/vpu")
def vpu() -> dict[str, Any]:
    return _cache["vpu"]


@app.get("/")
def index() -> FileResponse:
    return FileResponse(FRONTEND / "index.html")


app.mount("/static", StaticFiles(directory=str(FRONTEND)), name="static")
