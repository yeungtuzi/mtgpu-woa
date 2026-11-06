#!/bin/bash
# test-vdisplay.sh — 实验：M1000 vGPU 独占显示，通过 vfio-pci-sanway 的
# listen-socket 把 scanout 的 dmabuf 交给主机（不经 virtio-gpu / QEMU 显示后端）。
#
# 这是对 "sanway-boot-display 只管 boot，运行时由 M1000 直出" 这一猜想的验证。
# 失败可用 /tmp/winvm-qemu-cmdline.txt 里的上游命令行回退。
set -e

B=/opt/mtvmm/vendor/bin/ecoengine-aarch64
IMG=/home/lf/win11-arm64.qcow2
SOCKDIR=/tmp

U=$(ls /sys/bus/mdev/devices/ 2>/dev/null | head -1)
if [ -z "$U" ]; then echo "no mdev present; refusing to create one here"; exit 1; fi
echo "using existing mdev: $U  (never remove it -- vgpu_release panics the host)"

# 停掉正在跑的上游 QEMU 与旧 viewer（干净退出，mdev 保留）
pkill -f "opt/mtvmm/bin/winvm-viewer" 2>/dev/null || true
if pgrep -f qemu-system-aarch64 >/dev/null; then
  echo "stopping current qemu (ACPI powerdown) ..."
  python3 - <<'PY' || true
import socket, time
s = socket.socket(socket.AF_UNIX); s.connect('/tmp/qemudisplay.sock'); s.settimeout(5)
time.sleep(0.3)
try: s.recv(65536)
except Exception: pass
s.sendall(b'{"execute":"system_powerdown"}\n')
time.sleep(1); s.close()
PY
  for i in $(seq 1 60); do pgrep -f qemu-system-aarch64 >/dev/null || break; sleep 1; done
  if pgrep -f qemu-system-aarch64 >/dev/null; then echo "qemu did not exit; abort"; exit 1; fi
  echo "qemu exited"
fi

rm -f "$SOCKDIR"/qemumdev.sock "$SOCKDIR"/qemudod.sock "$SOCKDIR"/qemudisplay.sock

# 关键差异：没有 virtio-gpu，显示只可能来自 M1000 的 virtual display。
"$B" -L /opt/mtvmm/vendor/share/qemu -M virt -accel kvm -cpu host -m 8192 -smp 8 \
  -bios /opt/mtvmm/vendor/share/qemu/QEMU_EFI.fd \
  -drive file="$IMG",if=none,id=hd0,format=qcow2,cache=writeback \
  -device virtio-blk-pci,drive=hd0,addr=0xa \
  -nic type=user,model=virtio,hostfwd=tcp:127.0.0.1:8790-:8790 \
  -device qemu-xhci,addr=0x7 -device usb-tablet -device usb-kbd \
  -device vfio-pci-sanway,sysfsdev=/sys/bus/mdev/devices/"$U",listen-socket="$SOCKDIR"/qemumdev.sock,addr=0x6 \
  -display none \
  -qmp unix:"$SOCKDIR"/qemudisplay.sock,server,nowait \
  -daemonize -pidfile /tmp/winvm-qemu.pid > /tmp/winvm-qemu.log 2>&1

sleep 3
chmod 666 "$SOCKDIR"/qemumdev.sock 2>/dev/null || true
echo "qemu up: pid=$(cat /tmp/winvm-qemu.pid 2>/dev/null)"
ls -la "$SOCKDIR"/qemumdev.sock 2>/dev/null || echo "NO listen socket -- device did not come up"
