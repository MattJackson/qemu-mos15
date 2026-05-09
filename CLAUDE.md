# mos-qemu — agent context

QEMU 11.0.0 fork. Three overlay files (`hw/misc/applesmc.c`,
`hw/display/vmware_vga.c`, `hw/usb/dev-hid.c`) and one new device
(`hw/display/apple-gfx-pci-linux.c` + `apple-gfx-common-linux.c` +
`apple-gfx-linux.h`). All four are PR-ready for `qemu-project/qemu`
upstream — see `upstream-pr/`.

## Status

**Plumbing-stable.** Patches won't be revisited at the architectural
level; expected churn is QEMU version bumps and small upstream-review
adjustments. The active mos frontier is *consumer-side*
(libapplegfx-vulkan opcode handlers); this repo's job is to stay
buildable, upstream-submittable, and correct against macOS 15.7.x.

## Project context lives in the broader project

This repo is one of six in mos. For project-wide context — current
milestone, working mode, the standing rules, the don't-do-this list,
the universal build/deploy commands — read these in order:

- `../mos/CLAUDE.md` — airport map, working mode, current state
- `../mos/memory/MEMORY.md` — standing rules + closed-milestone facts
- `../mos/paravirt-re/library/` — wire protocols, class layouts, state machines

Public-audience documentation is in **`../mos-docs/`**. When in doubt
about what to surface to an external reader, read `mos-docs/README.md`
and look up the relevant whitepaper or reference doc.

## This-repo standing rules

- **Upstream-clean code.** Every commit subject uses
  `hw/<subsystem>/<file>: <imperative>`. No `mos15`, `mos-qemu`,
  `MattJackson`, `mjackson`, or `pq.io` strings in C/H source or
  patch-added lines. CI scans for these (`forbidden-identifiers`
  job in `.github/workflows/ci.yml`).
- **No `Co-Authored-By: Claude` trailers**, anywhere. Per the user's
  global rule.
- **GPL-2.0-or-later** on QEMU-derived files (verbatim header).
  AGPL-3.0 on the repo as a whole. The combined work is compatible
  via AGPL-3.0 → GPL-3.0 → GPL-2.0-or-later.
- **Logging:** `qemu_log_mask(LOG_GUEST_ERROR, ...)`,
  `trace_*`, `warn_report`. No `printf` / bare `qemu_log` for
  runtime diagnostics.
- **Coding style:** 4-space indent, K&R braces, match
  `hw/display/` and `hw/misc/` cadence.

## Build / iteration loop

Production build is via `mos-docker`'s Dockerfile. Fast iteration on
a `.c` change without rebuilding the whole image:

1. Edit one `.c` file in `/Users/mjackson/Developer/qemu-mos15/hw/...`.
2. Build a single binary inside Alpine 3.21 (matches the runtime
   container's musl libc) — see "Standalone build" in `README.md`.
3. `scp qemu-system-x86_64-alpine docker:/path/in/container/...` then
   restart the mos container. The `mos-docker` repo's
   `docs/qemu-build.md` (if present) or the README's "Build
   environment" block walks the canonical path.

Test from the laptop with the four phase scripts at the repo root
(`launch-phase-{0,1,2,3,4}.sh`) — each does a `ssh docker 'docker
compose up -d qemu-phN'`. `validate-baseline.sh <phase>` does an
ImageMagick compare against `baselines/<phase>_success.png`.

## Gotchas

- **musl vs glibc:** A glibc-built binary copied into the Alpine
  3.21 runtime fails with a misleading `required file not found` —
  the missing file is the dynamic linker (`/lib64/ld-linux-x86-64.so.2`,
  not present on musl). `file qemu-system-x86_64` should show
  `interpreter /lib/ld-musl-x86_64.so.1`. Always build inside Alpine.
- **apple-gfx-pci has a hard configure-time dependency** on
  `libapplegfx-vulkan`. `hw/display/meson.build` declares it
  `required: true`. Was `required: false` until 2026-04-20; silent
  drop produced QEMU binaries that compiled fine but had no
  `apple-gfx-pci` device, then crashed at runtime. Always
  `meson install --prefix=/usr` libapplegfx-vulkan **before** running
  QEMU's `./configure`.
- **ROM layout / `--prefix`:** `pc-bios/apple-gfx-pci.rom` is loaded
  by QEMU's PCI core via `qemu_find_file()` at realize time. The
  search path comes from `qemu_datadir`, set by `--datadir` /
  `--prefix` at configure. If the binary is built with one prefix
  and run from another, the option ROM lookup misses and the device
  fails to realize. Override at the CLI:
  `-device apple-gfx-pci,romfile=/abs/path/to/apple-gfx-pci.rom`.
- **Alpine package names:** `dtc-dev` (not `libfdt-dev`); do **not**
  install `libmount-dev` (doesn't exist in Alpine 3.21 and isn't
  needed).
- **`#KEY` total count is computed at SMC realize.** When adding a
  key via `applesmc_add_key(...)`, the count auto-updates — no
  manual bookkeeping. But: if `GET_KEY_BY_INDEX` is ever stripped
  back to upstream, the retry-storm regresses immediately
  (1,800 errors/sec, kernel_task 70%, WindowServer 509%).

## When in doubt, read mos-docs

Public-quality documentation for this repo's surface area is in
`../mos-docs/`:

- `whitepapers/02-opcode-catalog.md` — what flows through
  `apple-gfx-pci`'s BAR0 doorbell
- `whitepapers/09-osk-and-board-id.md` — what each Apple identity
  fact gates (and why USB HID is *not* on the must-spoof list)
- `architecture/04-display-paths.md` — vmware-vga vs std-vga vs
  apple-gfx-pci, when to pick which
- `reference/smc-key-table.md` — every SMC key the patched
  `applesmc.c` handles, with encoding cheat sheet
- `reference/qemu-args.md` — every flag in `mos-docker`'s
  production launcher
- `reference/apple-gfx-pci-mmio.md` — BAR0 register layout, doorbell
  semantics, capability gate (`0x122c` ← 9)

Do NOT duplicate that content in this repo's `README.md` — link.
