# Architecture

**[中文](architecture.md)**

```
                       host (Linux, aarch64)
  ┌──────────────────────────────────────────────────────────────┐
  │                                                              │
  │   /usr/bin/qemu-system-aarch64                               │
  │     -M virt -accel kvm -cpu host -m 8192 -smp 8              │
  │     AAVMF pflash  ──────────────┐                            │
  │     virtio-blk-pci  ──────────┐ │                            │
  │     virtio-gpu-pci  ────────┐ │ │                            │
  │     vfio-pci (mdev) ──────┐ │ │ │                            │
  │     intel-hda             │ │ │ │                            │
  │                           │ │ │ │                            │
  │   -display dbus ──────────┼─┼─┼─┼──► private D-Bus socket     │
  │   -qmp unix:...sock ──────┼─┼─┼─┼──► QMP (input, power)      │
  └───────────────────────────┼─┼─┼─┼───────────────────────────┘
                              │ │ │ │
                              │ │ │ └──► /dev/mdev ... vGPU (3D)
                              │ │ └────► qcow2 root disk
                              │ └──────► UEFI vars
                              │
              ┌───────────────┴────────────────┐
              │  winvm-viewer (X11)            │
              │   - dbus display client        │
              │   - QMP keyboard/mouse         │
              │   - clipboard bridge           │
              │   - resolution follow          │
              └───────────────┬────────────────┘
                              │ TCP 8790 / 8791
                              ▼
                    guest agent + audio capture
```

## Why these choices

### Display: RDP + RemoteFX (default)

Display goes through Windows' own remote desktop stack: the guest encodes AVC444
with the GPU's hardware encoder, the host decodes in hardware. The reason is that
the protocol already provides what we need — clipboard, audio redirection,
dynamic resolution, drive redirection, IME — so the host does not have to
reimplement each one, and any RDP client with RemoteFX support can attach.

`-display dbus` with the bundled viewer remains as a fallback (`MODE=viewer`).
It carries the scanout buffer inline, which is exactly what an X11 `XPutImage`
wants, at the cost of moving every frame through host CPU.

Alternatives considered: `egl-headless` needs a GL-capable host GPU context and
gives you a texture, not a window; VNC works but is a second network protocol
and fixed-size; SPICE needs a whole server stack.

### Disk: `virtio-blk-pci`

`virtio-blk` with the stock `viostor` driver is the boring, fast, supported
option. Note: do **not** set
`disable-modern=on` — with a modern-only Windows driver the guest loads a few
megabytes and then stalls.

### GPU: plain `vfio-pci`

A MTGPU vGPU is exposed as a mediated device. `vfio-pci,sysfsdev=<uuid>`
attaches it like any other mdev; the guest binds the GPU vendor's Windows
driver and gets a normal D3D adapter. No QEMU-side device is required.

### Audio: `intel-hda` + `-audiodev pa`

HDA emulation plus a native PulseAudio backend is one line of QEMU and works
with the in-box Windows driver.

## Boot flow

1. `winvm start` launches QEMU (daemonised) and creates the mdev if needed.
2. AAVMF runs, Windows boots, the remote desktop service comes up.
3. Once the guest is reachable, `winvm` opens the RDP client against
   `127.0.0.1:3389` and logs in. The fallback path (`MODE=viewer`) starts the
   private D-Bus and the viewer instead.
4. On that fallback path the viewer paints the boot splash until the first frame
   arrives; `viogpudo` then sets a mode, QEMU sends `Scanout`, and the viewer
   starts blitting `Update` calls.
5. The guest agent comes up (~60–100 s in); clipboard and resolution are then
   synchronised by RDP (or by the agent on the fallback path).
