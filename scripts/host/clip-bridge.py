#!/usr/bin/env python3
# Bidirectional clipboard bridge: host (aibook, X11 clipboard) <-> guest (HTTP).
# GET  /clip -> current host clipboard (UTF-8)
# POST /clip -> set host clipboard from request body
import glob
import http.server
import os
import subprocess

DISPLAY = os.environ.get("DISPLAY", ":0")
XAUTH = os.environ.get("XAUTHORITY") or (glob.glob("/run/user/1000/xauth_*") or [""])[0]
ENV = dict(os.environ, DISPLAY=DISPLAY, XAUTHORITY=XAUTH)

def get_clip():
    try:
        return subprocess.run(["xclip", "-o", "-selection", "clipboard"],
                              env=ENV, capture_output=True, timeout=2).stdout.decode("utf-8", "replace")
    except Exception:
        return ""

def set_clip(data: bytes):
    try:
        subprocess.run(["xclip", "-in", "-selection", "clipboard"],
                       input=data, env=ENV, timeout=2)
    except Exception:
        pass

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass
    def do_GET(self):
        if self.path != "/clip":
            self.send_error(404); return
        body = get_clip().encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def do_POST(self):
        if self.path != "/clip":
            self.send_error(404); return
        n = int(self.headers.get("Content-Length", "0"))
        set_clip(self.rfile.read(n))
        self.send_response(200); self.end_headers()

if __name__ == "__main__":
    http.server.ThreadingHTTPServer(("0.0.0.0", 8765), Handler).serve_forever()
