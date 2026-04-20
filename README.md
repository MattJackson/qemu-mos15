# mos-qemu

QEMU source patches for running macOS 15 (Sequoia) as a guest in
QEMU/KVM. Three files modified from upstream QEMU 10.2.2, plus a
new `hw/display/apple-gfx-pci-linux` device that implements the
host side of Apple's **ParavirtualizedGraphics** protocol (the
`PGDevice` / `PGShellCallbacks` shape) on Linux, backed by
[libapplegfx-vulkan](https://github.com/MattJackson/libapplegfx-vulkan).

> **Status: v0.5 — usable but barebones.** Verified end-to-end on macOS 15.7.5 inside KVM on Linux. Patches are tracked here as standalone files and overlay-copied into a QEMU source tree before configure/make.

## What's in here

| File | Purpose | Why patched |
|---|---|---|
| `hw/misc/applesmc.c` | Apple SMC device emulation | Real `GET_KEY_BY_INDEX_CMD` (upstream returned 4 zero bytes, causing macOS to retry at ~1,800 errors/sec, burning `kernel_task` 70% + `WindowServer` 509% CPU). Adds `#KEY` total-count and ~80 realistic iMac20,1 sensor values from real-hardware reference data. Adds `HE2N`, `WDTC`, and graphics-power keys macOS reads on boot. |
| `hw/display/vmware_vga.c` | VMware SVGA II display device | Extends the upstream device with capability bits the modern macOS driver expects (FIFO, pitchlock, alpha blend, multimon). Lifts the resolution ceiling toward 4K. |
| `hw/usb/dev-hid.c` | USB HID devices | Adds three opt-in wrapper types - `apple-kbd` / `apple-mouse` / `apple-tablet` - that inherit from `usb-kbd` / `usb-mouse` / `usb-tablet` and advertise Apple vendor ID `0x05ac` with real Apple product IDs at USB enumeration. Prevents macOS's Keyboard Setup Assistant from blocking first boot. The base `usb-kbd` / `usb-mouse` / `usb-tablet` devices are byte-for-byte upstream; only the new opt-in types carry Apple identity. |

## Quick start

The patches overlay onto upstream QEMU 10.2.2 source:

```bash
# Get upstream
curl -sL https://download.qemu.org/qemu-10.2.2.tar.xz | tar xJ -C /tmp
cd /tmp/qemu-10.2.2

# Apply our patches
git clone https://github.com/MattJackson/mos-qemu /tmp/qemu-mos15
cp /tmp/qemu-mos15/hw/misc/applesmc.c       hw/misc/applesmc.c
cp /tmp/qemu-mos15/hw/display/vmware_vga.c  hw/display/vmware_vga.c
cp /tmp/qemu-mos15/hw/usb/dev-hid.c         hw/usb/dev-hid.c

# Configure + build (Alpine 3.21 musl — see Build environment below)
./configure --target-list=x86_64-softmmu --enable-kvm --enable-slirp \
            --enable-linux-aio --enable-cap-ng --enable-seccomp --enable-vnc \
            --disable-docs --disable-debug-info --disable-werror
make -j$(nproc)
```

For the integrated build, see [mos-docker](https://github.com/MattJackson/mos-docker)'s Dockerfile.

## Build environment — must match the runtime container's libc

The integrated runtime is Alpine Linux 3.21 (musl libc). **Do not** build QEMU on a glibc host (Ubuntu/Debian/RHEL) and copy the binary into the container — the binary fails to launch with a misleading `required file not found` error (the missing file is the dynamic linker `ld-linux-x86-64.so.2`, which doesn't exist on Alpine).

To do a fast iteration on these patches without rebuilding the whole image, build inside an Alpine container that matches the target:

```bash
docker run --rm -v /tmp/qemu-10.2.2:/src -v /tmp:/out alpine:3.21 sh -c '
  apk add --no-cache build-base python3 ninja meson pkgconf \
      glib-dev pixman-dev libcap-ng-dev libseccomp-dev \
      libslirp-dev libaio-dev dtc-dev curl bash
  cd /src && [ -d build-alpine ] || (mkdir build-alpine && cd build-alpine && ../configure ...)
  cd build-alpine && ninja qemu-system-x86_64
  cp qemu-system-x86_64 /out/qemu-mos15-alpine
'
```

Verify the resulting binary uses the right linker:

```bash
file qemu-mos15-alpine
# Expected: ELF 64-bit ... interpreter /lib/ld-musl-x86_64.so.1
```

Common Alpine-package gotchas: `dtc-dev` (not `libfdt-dev`); do **not** install `libmount-dev` (not needed and the package doesn't exist in Alpine 3.21).

Full iterate-build-deploy guide: [docker-macos/docs/qemu-mos15-build.md](https://github.com/MattJackson/mos-docker/blob/main/docs/qemu-mos15-build.md).

## SMC patch — design notes

`applesmc.c` is the most consequential patch in this repo. Background:

The upstream QEMU `applesmc` device implements just enough of the Apple SMC PMIO protocol to satisfy macOS's OSK boot check (the `OSK0` / `OSK1` keys with the well-known string). Beyond that, it returns canned zero bytes for most commands. macOS's modern `AppleSMC.kext` enumerates SMC keys by index at boot using `GET_KEY_BY_INDEX_CMD`. Upstream QEMU returns `00 00 00 00` for every index — an invalid key name. macOS treats this as `kSMCSpuriousData (0x81)` and retries forever.

Our patch:

1. Implements `GET_KEY_BY_INDEX_CMD` properly: parse the 4-byte big-endian index, walk the keys list, return the actual 4-character key name (or `APPLESMC_ST_1E_BAD_INDEX (0xb8)` for out-of-range — tells macOS to stop iterating).
2. Adds the `#KEY` special key — Apple SMC convention for "total key count". macOS reads it first to know the iteration upper bound.
3. Replaces zero-valued temperature/fan sensors with realistic iMac20,1 values. SP78 format (16-bit big-endian, top 8 bits = integer °C). fpe2 format for fan RPM (raw = RPM << 2).
4. Adds keys macOS reads on boot but upstream doesn't expose: `HE0N`, `HE2N` (graphics power), `WDTC` (watchdog), various platform/identity keys VirtualSMC also exposes for compatibility.

Measured impact in our VM:

| Metric | Before | After |
|---|---|---|
| SMC errors / 5s | 9,225 | 2 |
| `kernel_task` CPU | 70% | ~2% |
| `WindowServer` CPU | 509% | ~6% |

## Reference data

- `smc-keys-m2-macbookair.txt` — full SMC key dump from a real M2 MacBook Air (~63 KB, ~1700 keys). Reference for what a complete SMC implementation looks like.
- `smc-keys-vm-sequoia.txt` — much shorter dump from the VM, what macOS sees today.

## When upgrading QEMU versions

These three files are based on QEMU 10.2.2. To target a new QEMU version:

1. Pull the upstream version's source.
2. Diff each of our three files against upstream's same-version original. Resolve any merge conflicts.
3. Rebuild + run the [docker-macos test runbook](https://github.com/MattJackson/mos-docker/blob/main/docs/test-runbook.md).

## Status

**v0.5** — runtime-verified on QEMU 10.2.2 + KVM + Linux + Sequoia 15.7.5 guest. Known limitations:

- `applesmc.c` only emulates the keys macOS actively reads. Adding new keys is a one-line `applesmc_add_key(s, "KEYS", len, value)` call; auto-recompute of `#KEY` total handles count.
- VMware SVGA caps guest-visible VRAM at 512 MB even when we request 4096. Higher requires deeper edits to the device model — open enhancement.
- Single-display only. Multi-monitor support not validated.
- Sensor values are static — no dynamic temperature/fan curves. macOS doesn't seem to mind, but a future-real-feeling implementation would tie these to host metrics.

## Part of the mos suite

- [mos-docker](https://github.com/MattJackson/mos-docker) — orchestration: Docker image, build pipeline, kexts, runbook
- [mos-patcher](https://github.com/MattJackson/mos-patcher) — kernel-side hook framework (Lilu replacement)
- [mos-opencore](https://github.com/MattJackson/mos-opencore) — OpenCore config + patches
- [libapplegfx-vulkan](https://github.com/MattJackson/libapplegfx-vulkan) — Linux `ParavirtualizedGraphics` (PGDevice) host library; consumed by this repo's `hw/display/apple-gfx-pci-linux` device

## Keywords

macOS QEMU Linux, macOS 15 Sequoia QEMU, applesmc QEMU,
Apple ParavirtualizedGraphics QEMU device, PGDevice Linux,
apple-gfx-pci Linux port, paravirt GPU Linux, iMac20,1 VM.

## License

[GNU AGPL-3.0](LICENSE). The patched files are derivative works of QEMU (GPL-2.0-or-later); the combined work satisfies both since AGPL-3.0 implies GPL-3.0, which is compatible with GPL-2.0-or-later. Unmodified upstream QEMU files retain their original GPL-2.0-or-later licensing.
