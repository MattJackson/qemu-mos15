# qemu-mos15

QEMU source patches for macOS 15 (Sequoia) VMs. Three files modified from QEMU 10.2.2.

## Modified Files

| File | What | Why |
|------|------|-----|
| `hw/misc/applesmc.c` | Complete Apple SMC with 21 keys | Replaces VirtualSMC kext. Fixes AGPM crash (HE2N), watchdog errors (WDTC), provides GPU/fan/platform identity. |
| `hw/display/vmware_vga.c` | Modern VMware SVGA II device | 4K resolution (3840x2160), extended capabilities (FIFO, pitchlock, alpha blend, multimon). |
| `hw/usb/dev-hid.c` | Apple USB device identity | Keyboard/mouse/trackpad present as Apple devices (vendor 0x05ac). No Keyboard Setup Assistant. |

## Usage

These files overlay the QEMU 10.2.2 source during Docker build:

```dockerfile
RUN curl -sL https://download.qemu.org/qemu-10.2.2.tar.xz | tar xJ -C /tmp \
    && cd /tmp/qemu-10.2.2 \
    && cp /patches/hw/misc/applesmc.c hw/misc/applesmc.c \
    && cp /patches/hw/display/vmware_vga.c hw/display/vmware_vga.c \
    && cp /patches/hw/usb/dev-hid.c hw/usb/dev-hid.c \
    && ./configure --target-list=x86_64-softmmu ... \
    && make -j$(nproc)
```

## QEMU Version

Based on QEMU 10.2.2. When upgrading to a new QEMU version, diff these files against the upstream versions and merge.

## Part of mos15

- [docker-macos](https://github.com/MattJackson/docker-macos) — Docker image
- [qemu-mos15](https://github.com/MattJackson/qemu-mos15) — QEMU patches (this repo)
- [opencore-mos15](https://github.com/MattJackson/opencore-mos15) — OpenCore patches
