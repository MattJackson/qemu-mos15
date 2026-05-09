# Changelog

All notable changes to this fork are documented here. The format is based
on [Keep a Changelog 1.1](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

This changelog tracks the state of **this fork** (mos-qemu — a
patch-carrying fork of [qemu-project/qemu](https://gitlab.com/qemu-project/qemu)).
Upstream QEMU maintains its own release cadence and changelog; entries here
describe only the delta introduced by this fork on top of a specific
upstream base.

## [Unreleased]

_Nothing yet._

## [0.6.0] — 2026-05-XX

### Changed
- Rebased onto QEMU 11.0.0. Build infrastructure (Dockerfile `ARG
  QEMU_VERSION`, `ADD https://download.qemu.org/qemu-*.tar.xz` URL),
  documentation, and standalone-build recipe updated in lockstep.
  Patch surface unchanged (same three overlays + `apple-gfx-pci-linux`
  device); the rebase is mechanical against the new upstream base.

## [0.5.0] — 2026-04

First usable cut. Verified end-to-end on macOS 15.7.5 as a KVM guest on
Linux, built against QEMU 10.2.2.

### Added
- `hw/misc/applesmc.c`: real implementation of `GET_KEY_BY_INDEX_CMD`
  (parses the 4-byte big-endian index, walks the key list, returns the
  actual 4-character key name, or `APPLESMC_ST_1E_BAD_INDEX (0xb8)` for
  out-of-range indices).
- `hw/misc/applesmc.c`: `#KEY` total-count special key so macOS knows the
  iteration upper bound.
- `hw/misc/applesmc.c`: ~80 realistic iMac20,1 sensor values (SP78 for
  temperatures, fpe2 for fan RPM) replacing upstream's zero-valued
  defaults.
- `hw/misc/applesmc.c`: boot-required keys macOS reads but upstream did
  not expose — `HE0N`, `HE2N` (graphics power), `WDTC` (watchdog), and
  platform / identity keys matched against VirtualSMC for compatibility.
- `hw/display/vmware_vga.c`: capability bits the modern macOS VMware
  SVGA II driver expects (FIFO, pitchlock, alpha blend, alpha cursor,
  8-bit emulation, multi-monitor). Resolution ceiling lifted toward 4K.
- `hw/usb/dev-hid.c`: three opt-in wrapper device types — `apple-kbd`,
  `apple-mouse`, `apple-tablet` — that inherit from
  `usb-kbd` / `usb-mouse` / `usb-tablet` and advertise Apple vendor ID
  `0x05ac` with real Apple product IDs at USB enumeration, so macOS
  guests don't block on Keyboard Setup Assistant at first boot. Base
  devices remain byte-for-byte upstream.
- `hw/display/apple-gfx-pci-linux/`: new device implementing the host
  side of Apple's ParavirtualizedGraphics protocol (`PGDevice` /
  `PGShellCallbacks`) on Linux, backed by
  [libapplegfx-vulkan](https://github.com/MattJackson/libapplegfx-vulkan).
- `upstream-pr/`: four staged patch series prepared for submission to
  qemu-project/qemu upstream (applesmc fix, vmware_vga caps, USB HID
  Apple IDs, apple-gfx-pci Linux port).

### Fixed
- `hw/misc/applesmc.c`: upstream `GET_KEY_BY_INDEX_CMD` returned four
  zero bytes for every index, which macOS's modern `AppleSMC.kext`
  interpreted as `kSMCSpuriousData (0x81)` and retried on indefinitely.
  Measured impact in-VM: SMC errors/5s dropped 9,225 to 2;
  `kernel_task` CPU 70% to ~2%; `WindowServer` CPU 509% to ~6%.

### Notes
- Patches are tracked in this fork as standalone files and overlay-copied
  onto an unpacked QEMU 10.2.2 source tree before `configure` / `make`.
  See `README.md` for the build recipe.
- Licensing: this fork is AGPL-3.0; QEMU-derived files retain their
  upstream GPL-2.0-or-later licensing. The combined work satisfies both
  (AGPL-3.0 implies GPL-3.0, which is compatible with GPL-2.0-or-later).

[Unreleased]: https://github.com/MattJackson/mos-qemu/compare/v0.6.0...HEAD
[0.6.0]: https://github.com/MattJackson/mos-qemu/releases/tag/v0.6.0
[0.5.0]: https://github.com/MattJackson/mos-qemu/releases/tag/v0.5.0
