#!/bin/bash
# 托盘守护：python 挂了就拉起来。宿主画面是唯一交互界面，托盘本身不能默默消失。
export DISPLAY="${DISPLAY:-:0}"
if [ -z "${XAUTHORITY:-}" ]; then
  XAUTHORITY=$(ls /run/user/1000/xauth_* 2>/dev/null | head -1)
  export XAUTHORITY
fi
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
for attempt in $(seq 1 100); do
  python3 /usr/local/lib/winvm/winvm-tray.py >>/tmp/winvm-tray.log 2>&1
  echo "$(date -Is) tray exited rc=$?, restarting" >>/tmp/winvm-tray.log
  sleep 2
done
