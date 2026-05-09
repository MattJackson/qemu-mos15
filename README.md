# mos-qemu

QEMU 11.0.0 patches for running unmodified macOS 15 (Sequoia) as a
guest on Linux + KVM. Three overlay files plus one new device that
implements the host side of Apple's **ParavirtualizedGraphics**
protocol on Linux — the missing piece that lets `apple-gfx-pci`
drive Metal-via-Vulkan/lavapipe instead of Apple's framework.

This repo is one of six in the **[mos](https://github.com/MattJackson/mos)**
project (open-source macOS-on-Linux with Apple's paravirt GPU). The
curated narrative — vision, architecture, whitepapers, reference —
lives in **[mos-docs](https://github.com/MattJackson/mos-docs)**.
This README covers what `mos-qemu` ships and how to build it.

> **Status — v0.5, plumbing-stable.** Verified end-to-end on macOS
> 15.7.5 inside KVM on Linux. The patches are stable and won't be
> revisited at the architectural level. The ongoing work is
> *consumer-side* (libapplegfx-vulkan opcode handlers); see
> [project status](https://github.com/MattJackson/mos-docs/blob/main/overview/project-status.md).

## Patch surface

Four files vs upstream QEMU 11.0.0 — three overlays and one new device:

| File | What it adds | Whitepaper / reference |
|---|---|---|
| `hw/misc/applesmc.c` | Real `GET_KEY_BY_INDEX_CMD` (upstream returned 4 zero bytes → macOS retried at 1,800 errors/sec, burning `kernel_task` 70% + `WindowServer` 509% CPU). Adds `#KEY` total-count and ~80 realistic iMac20,1 sensor values; adds `HE0N`/`HE2N`/`WDTC` and other boot-required keys. | [whitepaper 09 — OSK and board id](https://github.com/MattJackson/mos-docs/blob/main/whitepapers/09-osk-and-board-id.md) · [SMC key table](https://github.com/MattJackson/mos-docs/blob/main/reference/smc-key-table.md) |
| `hw/display/vmware_vga.c` | Capability bits the modern macOS VMware SVGA II driver expects (PITCHLOCK, EXTENDED_FIFO, 8BIT_EMULATION, ALPHA_BLEND, ALPHA_CURSOR, multi-monitor). Resolution ceiling raised toward 5K. | [display paths](https://github.com/MattJackson/mos-docs/blob/main/architecture/04-display-paths.md) |
| `hw/usb/dev-hid.c` | Three opt-in wrapper types — `apple-kbd` / `apple-mouse` / `apple-tablet` — that inherit from `usb-kbd` / `usb-mouse` / `usb-tablet` and advertise Apple vendor `0x05ac` with real Apple product IDs at USB enumeration. Avoids macOS's Keyboard Setup Assistant blocking first boot. Base devices are byte-for-byte upstream. | [whitepaper 09 — OSK and board id](https://github.com/MattJackson/mos-docs/blob/main/whitepapers/09-osk-and-board-id.md) (HID descriptor caveats) |
| `hw/display/apple-gfx-pci-linux.c` (+ `apple-gfx-common-linux.c`, `apple-gfx-linux.h`) | New device. Linux C port of upstream's macOS-only `apple-gfx-pci.m`; implements the `PGDevice` / `PGShellCallbacks` shape against [libapplegfx-vulkan](https://github.com/MattJackson/libapplegfx-vulkan) (Mesa lavapipe backend) instead of Apple's framework. Exposes PCI `0x106B:0xEEEE`, BAR0 MMIO + MSI, `gpu_cores=N` tunable, configurable display modes. | [whitepaper 02 — opcode catalog](https://github.com/MattJackson/mos-docs/blob/main/whitepapers/02-opcode-catalog.md) · [BAR0 MMIO map](https://github.com/MattJackson/mos-docs/blob/main/reference/apple-gfx-pci-mmio.md) |

Also touched: `hw/display/meson.build`, `hw/display/Kconfig`,
`hw/display/trace-events`, `pc-bios/meson.build` (build wiring for
the new device); `pc-bios/apple-gfx-pci.rom` (option ROM placeholder
shipped at runtime).

For the byte-level production launcher arguments that exercise these
patches, see [reference/qemu-args.md](https://github.com/MattJackson/mos-docs/blob/main/reference/qemu-args.md).

## How to use this repo

There are three modes:

### (a) Consumed by mos-docker (recommended)

You don't build by hand — [mos-docker](https://github.com/MattJackson/mos-docker)'s
Dockerfile fetches upstream QEMU 11.0.0, overlays this repo's files,
and builds in a single Alpine 3.21 stage. Two commands from a fresh
Linux host get you a running macOS guest. Start at the
[mos-docker README](https://github.com/MattJackson/mos-docker).

### (b) Standalone build (for testing patches)

To validate a patch change without rebuilding the full container,
build inside an Alpine 3.21 (musl) container that matches the
runtime libc:

```bash
# 1. Fetch upstream QEMU
curl -sL https://download.qemu.org/qemu-11.0.0.tar.xz | tar xJ -C /tmp
cd /tmp/qemu-11.0.0

# 2. Overlay this repo's files
git clone https://github.com/MattJackson/mos-qemu /tmp/mos-qemu
cp /tmp/mos-qemu/hw/misc/applesmc.c                  hw/misc/
cp /tmp/mos-qemu/hw/display/vmware_vga.c             hw/display/
cp /tmp/mos-qemu/hw/usb/dev-hid.c                    hw/usb/
cp /tmp/mos-qemu/hw/display/apple-gfx-pci-linux.c    hw/display/
cp /tmp/mos-qemu/hw/display/apple-gfx-common-linux.c hw/display/
cp /tmp/mos-qemu/hw/display/apple-gfx-linux.h        hw/display/
cp /tmp/mos-qemu/hw/display/meson.build              hw/display/
cp /tmp/mos-qemu/hw/display/Kconfig                  hw/display/
cp /tmp/mos-qemu/hw/display/trace-events             hw/display/
cp /tmp/mos-qemu/pc-bios/meson.build                 pc-bios/
cp /tmp/mos-qemu/pc-bios/apple-gfx-pci.rom           pc-bios/

# 3. Build in Alpine 3.21 (matches the runtime container's musl libc)
docker run --rm -v /tmp/qemu-11.0.0:/src -v /tmp:/out alpine:3.21 sh -c '
  apk add --no-cache build-base python3 ninja meson pkgconf bash curl \
      glib-dev pixman-dev libcap-ng-dev libseccomp-dev \
      libslirp-dev libaio-dev dtc-dev
  # libapplegfx-vulkan must be installed BEFORE configure for apple-gfx-pci.
  # Build + install it from source (also AGPL-3.0):
  apk add --no-cache git mesa-dev vulkan-headers vulkan-loader-dev mesa-vulkan-swrast
  git clone https://github.com/MattJackson/libapplegfx-vulkan /tmp/libapplegfx
  cd /tmp/libapplegfx && meson setup --prefix=/usr build && ninja -C build install
  # Now configure + build QEMU
  cd /src && mkdir -p build-alpine && cd build-alpine && \
    ../configure --target-list=x86_64-softmmu --enable-kvm --enable-slirp \
                 --enable-linux-aio --enable-cap-ng --enable-seccomp --enable-vnc \
                 --disable-docs --disable-debug-info --disable-werror && \
    ninja qemu-system-x86_64 && cp qemu-system-x86_64 /out/qemu-system-x86_64-alpine
'

# 4. Verify musl linkage
file /tmp/qemu-system-x86_64-alpine
# Expected: ELF 64-bit ... interpreter /lib/ld-musl-x86_64.so.1
```

The canonical recipe lives in
[mos-docker's Dockerfile](https://github.com/MattJackson/mos-docker/blob/main/Dockerfile)
builder stage; the steps above are that recipe distilled for an ad-hoc shell.

#### Build environment must match the runtime container's libc

If you run `mos-qemu` inside the production Alpine 3.21 container,
**the binary must also be built on Alpine 3.21**. Copying a glibc-built
`qemu-system-x86_64` (Ubuntu / Debian / RHEL) into the container fails
with a misleading `required file not found` — the missing file is the
dynamic linker `/lib64/ld-linux-x86-64.so.2`, which doesn't exist on
musl. Verify with `file <binary>` (`/lib/ld-musl-x86_64.so.1` is
correct).

Common Alpine-package gotchas: `dtc-dev` (not `libfdt-dev`); do **not**
install `libmount-dev` (not needed; the package doesn't exist in
Alpine 3.21).

#### apple-gfx-pci has a hard configure-time dependency

`hw/display/meson.build` declares `dependency('libapplegfx-vulkan',
required: true)`. If pkg-config cannot find libapplegfx-vulkan when
the device is enabled, meson hard-fails. This is deliberate (was
`required: false` until 2026-04-20; silent-drop produced QEMU
binaries that compiled fine but had no `apple-gfx-pci` device, then
crashed at runtime). Build and install
[libapplegfx-vulkan](https://github.com/MattJackson/libapplegfx-vulkan)
to a `--prefix` whose `lib/pkgconfig/` is on `PKG_CONFIG_PATH`
**before** running `./configure`.

### (c) Upstream submission staging

The patches in this fork are written as if submitting to
qemu-project/qemu tomorrow. Each upstream-destined change lives in
`upstream-pr/<topic>/` as a `git format-patch` series with cover
letter, testing recipe, and PR description. Current state:

- **Package A — applesmc fix** (4 patches): ready to submit
- **Package C — vmware_vga capabilities** (4 patches): ready to submit
- **Package D — USB HID Apple wrapper devices** (3 patches): ready to submit
- **Package B — apple-gfx-pci Linux port** (9 patches): ready
  pending a packaging-path decision for `libapplegfx-vulkan`
  (system package vs `subprojects/` submodule vs vendored — see
  [`upstream-pr/apple-gfx-pci-linux/LIBAPPLEGFX_DEPENDENCY.md`](upstream-pr/apple-gfx-pci-linux/LIBAPPLEGFX_DEPENDENCY.md))

CI ([`.github/workflows/ci.yml`](.github/workflows/ci.yml)) gates on:
overlay files compile against pristine upstream 11.0.0; every
`upstream-pr/*/` series applies cleanly with `git am`; no fork-local
identifiers (`mos15`, `mos-qemu`, etc.) leak into upstream-destined
code or patch bodies; no QEMU-10-removed API patterns.

## Pin and rebase strategy

This fork pins **upstream QEMU 11.0.0**. The same version is pinned
by `mos-docker`'s Dockerfile (`ARG QEMU_VERSION`) and the CI
workflow's `env.QEMU_VERSION`; bump all three in lockstep.

To rebase onto a newer QEMU release:

1. Fetch the new upstream tarball and unpack alongside this tree.
2. Diff each of our four patched / new files against the new
   upstream same-named file. `applesmc.c`, `vmware_vga.c`, `dev-hid.c`
   typically rebase cleanly — these are localized changes; merge-conflict
   noise tends to be in unrelated upstream-side helper functions.
3. `apple-gfx-pci-linux.c` is a new file — needs only meson/Kconfig
   compatibility checks against the new tree.
4. Rebuild the test image and run the regression suite (see
   [mos-docker's regression tests](https://github.com/MattJackson/mos-docker#regression-testing)).
5. Update `CHANGELOG.md` and the version pin in mos-docker + CI.

## Sibling repos

| Repo | Role |
|---|---|
| [mos](https://github.com/MattJackson/mos) | Project meta-repo: RE notes (`paravirt-re/`), kext sources, milestones, project memory |
| [mos-docs](https://github.com/MattJackson/mos-docs) | Documentation library: vision, architecture, whitepapers, reference |
| [mos-docker](https://github.com/MattJackson/mos-docker) | Runtime — Docker image that consumes this repo's binaries to boot macOS |
| [libapplegfx-vulkan](https://github.com/MattJackson/libapplegfx-vulkan) | Host PVG library that `apple-gfx-pci-linux` links against (Vulkan / lavapipe) |
| [mos-patcher](https://github.com/MattJackson/mos-patcher) | Lilu-replacement kext (per-instance vtable swap on Sequoia) |
| [mos-opencore](https://github.com/MattJackson/mos-opencore) | OpenCore EFI image build script |

The on-disk repo names (`qemu-mos15`, `mos15-patcher`, `opencore-mos15`)
are local shorthands; the public names above are canonical.

## Reference data shipped in-tree

- `smc-keys-m2-macbookair.txt` — full SMC key dump from a real
  M2 MacBook Air (~63 KB, ~1700 keys). Reference for what a
  complete SMC implementation looks like.
- `smc-keys-vm-sequoia.txt` — much shorter dump from the VM,
  what macOS sees today.
- `pc-bios/apple-gfx-pci.rom` — extracted `AppleParavirtEFI.rom`
  (~16 KB) loaded by QEMU's PCI core into the device's ROM BAR.
  Slated for replacement by an in-tree EDK2 build.

## Project status

The plumbing-side state of this repo is **stable**: no open
architectural questions, no expected revisits except patch
maintenance during QEMU version bumps. The frontier of mos work is
*consumer-side* — [`libapplegfx-vulkan`](https://github.com/MattJackson/libapplegfx-vulkan)
opcode handlers (the [95 Render + 32 Compute + 24 Blit ops](https://github.com/MattJackson/mos-docs/blob/main/whitepapers/02-opcode-catalog.md)
that flow through `apple-gfx-pci`'s BAR0 doorbell). Top-line state
across the six repos lives at
[mos-docs/overview/project-status.md](https://github.com/MattJackson/mos-docs/blob/main/overview/project-status.md).

## License

[GNU AGPL-3.0](LICENSE) on additions in this repo. QEMU-derived files
(`applesmc.c`, `vmware_vga.c`, `dev-hid.c`) retain upstream's
GPL-2.0-or-later headers verbatim — the combined work satisfies
both via the AGPL-3.0 → GPL-3.0 → GPL-2.0-or-later compatibility
chain. The new `apple-gfx-pci-linux.c` carries `SPDX-License-Identifier:
GPL-2.0-or-later` matching upstream's `apple-gfx-pci.m`, in
preparation for upstream submission.

See also: [`CONTRIBUTING.md`](CONTRIBUTING.md) (upstream-submission
discipline, commit style), [`SECURITY.md`](SECURITY.md) (advisories),
[`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md), [`CHANGELOG.md`](CHANGELOG.md).

## Keywords

macOS QEMU Linux, macOS 15 Sequoia QEMU, applesmc QEMU,
Apple ParavirtualizedGraphics QEMU device, PGDevice Linux,
apple-gfx-pci Linux port, paravirt GPU Linux, iMac20,1 VM.
