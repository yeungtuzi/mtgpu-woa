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
WINVM = "/usr/local/bin/winvm"
CLIP_BRIDGE = "http://127.0.0.1:8765/clip"
PORT = 8760

DEFAULT_SETTINGS = {
    "width": 1920,
    "height": 1080,
    "fps": 60,
    "bitrate": 20000,
    "app": "Desktop",
}


def load_settings() -> dict[str, Any]:
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    if SETTINGS_FILE.exists():
        try:
            data = json.loads(SETTINGS_FILE.read_text())
            return {**DEFAULT_SETTINGS, **data}
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


class SettingsRequest(BaseModel):
    width: int | None = None
    height: int | None = None
    fps: int | None = None
    bitrate: int | None = None
    app: str | None = None


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
    width = req.width or settings["width"]
    height = req.height or settings["height"]
    fps = req.fps or settings["fps"]
    bitrate = req.bitrate or settings["bitrate"]
    if stream_running():
        return JSONResponse({"ok": False, "error": "已有串流在运行，请先停止"}, status_code=409)
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
    ]
    try:
        _stream_proc = subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
            env=display_env(),
        )
    except Exception as exc:  # pragma: no cover
        return JSONResponse({"ok": False, "error": str(exc)}, status_code=500)
    _stream_info = {"app": app_name, "pid": _stream_proc.pid, "started": time.time()}
    save_settings({"app": app_name, "width": width, "height": height, "fps": fps, "bitrate": bitrate})
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


@app.get("/api/settings")
def get_settings() -> dict[str, Any]:
    return load_settings()


@app.put("/api/settings")
def put_settings(req: SettingsRequest) -> dict[str, Any]:
    data = {k: v for k, v in req.model_dump().items() if v is not None}
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
