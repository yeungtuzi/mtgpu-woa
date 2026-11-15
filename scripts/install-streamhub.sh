#!/usr/bin/env bash
# Install StreamHub (backend + web client) into /opt/mtgpu-woa/streamhub and
# add a desktop launcher that opens it in a Chrome app window.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
if [ -d "$here/../apps/streamhub" ]; then
  src=$(cd "$here/../apps/streamhub" && pwd)
elif [ -d "$here/apps/streamhub" ]; then
  src=$(cd "$here/apps/streamhub" && pwd)
else
  echo "streamhub sources not found next to $here" >&2
  exit 1
fi

dest=/opt/mtgpu-woa/streamhub
sudo install -d "$dest"
sudo cp -a "$src/." "$dest/"
sudo chown -R "$(id -u):$(id -g)" "$dest"

cat > "$dest/streamhub.sh" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
export DISPLAY="${DISPLAY:-:0}"
if [ -z "${XAUTHORITY:-}" ]; then
  XAUTHORITY=$(ls /run/user/"$(id -u)"/xauth_* 2>/dev/null | head -1 || true)
  export XAUTHORITY
fi
PORT=8760
if ! curl -sf --max-time 1 "http://127.0.0.1:$PORT/api/status" >/dev/null; then
  cd /opt/mtgpu-woa/streamhub
  nohup python3 -m uvicorn backend.app:app --host 127.0.0.1 --port "$PORT" \
    >> /opt/mtgpu-woa/streamhub/streamhub.log 2>&1 &
  for _ in $(seq 1 60); do
    curl -sf --max-time 1 "http://127.0.0.1:$PORT/api/status" >/dev/null && break
    sleep 0.2
  done
fi
exec google-chrome \
  --app="file:///opt/mtgpu-woa/streamhub/frontend/index.html" \
  --window-size=1280,860 \
  --class=StreamHub \
  --user-data-dir="$HOME/.config/streamhub-chrome" \
  --no-first-run --no-default-browser-check
SH
chmod +x "$dest/streamhub.sh"

cat > "$dest/icon.svg" <<'SVG'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128">
  <defs>
    <linearGradient id="g" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#5eead4"/>
      <stop offset="0.55" stop-color="#7dd3fc"/>
      <stop offset="1" stop-color="#a78bfa"/>
    </linearGradient>
  </defs>
  <rect width="128" height="128" rx="30" fill="#071019"/>
  <circle cx="64" cy="58" r="34" fill="none" stroke="url(#g)" stroke-width="10"/>
  <path d="M52 48 L86 62 L52 76 Z" fill="url(#g)"/>
</svg>
SVG

install -d "$HOME/.local/share/applications"
cat > "$HOME/.local/share/applications/streamhub.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=StreamHub
Comment=MTGPU Moonlight/Sunshine control client
Exec=$dest/streamhub.sh
Icon=$dest/icon.svg
Terminal=false
Categories=Network;Game;
EOF

echo "installed: $dest"
echo "launcher: $HOME/.local/share/applications/streamhub.desktop"
echo "start:    $dest/streamhub.sh"
