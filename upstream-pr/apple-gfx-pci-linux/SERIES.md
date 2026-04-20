# hw/display: add Linux-host port of apple-gfx-pci

Upstream QEMU (since Phil Dennis-Jordan's series in 2024)
carries `hw/display/apple-gfx-pci.m` and `hw/display/apple-gfx.m`,
which implement the host side of Apple's
ParavirtualizedGraphics protocol. Those files are Objective-C
and depend on Apple's ParavirtualizedGraphics.framework; they
are therefore restricted to macOS hosts (`system_ss.add(when:
[pvg, ...])` in `hw/display/meson.build`).

This series adds a companion Linux C port that drives the
same PGDevice/PGShellCallbacks protocol via a library we
call `libapplegfx-vulkan`, backed by Mesa lavapipe. The
guest-side driver (Apple's `AppleParavirtGPU.kext`) is
unchanged; Linux hosts can now run macOS VMs with the same
guest kext that Apple ships, without needing a macOS host.

No upstream file is modified. The new files live alongside
the existing Objective-C implementation:

| File | Status |
|------|--------|
| `hw/display/apple-gfx.m`          | (upstream, untouched) |
| `hw/display/apple-gfx-pci.m`      | (upstream, untouched) |
| `hw/display/apple-gfx-mmio.m`     | (upstream, untouched) |
| `hw/display/apple-gfx.h`          | (upstream, untouched) |
| `hw/display/apple-gfx-pci-linux.c`   | **new (Linux port)** |
| `hw/display/apple-gfx-common-linux.c`| **new (Linux port)** |
| `hw/display/apple-gfx-linux.h`       | **new (Linux port)** |
| `hw/display/meson.build`             | **modified** (build gate) |
| `hw/display/Kconfig`                 | **modified** (new symbol) |
| `pc-bios/meson.build`                | **modified** (ship ROM) |
| `pc-bios/apple-gfx-pci.rom`          | **new blob** |

## Patch list

| Patch | Subject |
|-------|---------|
| 1/8   | `hw/display: add apple-gfx-pci-linux port skeleton` |
| 2/8   | `hw/display/apple-gfx-linux: memory/task plumbing` |
| 3/8   | `hw/display/apple-gfx-linux: MMIO dispatch` |
| 4/8   | `hw/display/apple-gfx-linux: display callbacks and console integration` |
| 5/8   | `hw/display/apple-gfx-linux: PCI device wrapper` |
| 6/8   | `hw/display/apple-gfx-linux: add gpu_cores property` |
| 7/8   | `hw/display/apple-gfx-linux: meson + Kconfig wiring` |
| 8/8   | `pc-bios: ship apple-gfx-pci option ROM` |

## Dependency

**Hard blocker:** this series depends on `libapplegfx-vulkan`
being discoverable via `pkg-config`. The library is not yet
distributed as a package; upstream submission is therefore
blocked on packaging of the dependency or a decision about
how to bundle it. See `LIBAPPLEGFX_DEPENDENCY.md`.

## Status

**Draft.** Submission gated on:

  1. `libapplegfx-vulkan` being either accepted as a system
     package (Debian/Fedora/Alpine) or bundled as a submodule
     under `subprojects/`.
  2. The `pc-bios/apple-gfx-pci.rom` blob being replaced with
     an in-tree EDK2 build (currently uses a blob extracted
     from Apple's framework, which is not redistributable
     under GPLv2).
  3. Sign-off from the apple-gfx.m author on the split: this
     port is structured to avoid any changes to
     `apple-gfx.m` by intent, but a review pass would catch
     places where the core C logic should be pulled out of
     `apple-gfx.m` into a shared `apple-gfx-common.c`.

See `LIBAPPLEGFX_DEPENDENCY.md` for the dependency story and
`TESTING.md` for how to build and run the series.
