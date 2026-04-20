# hw/display: add Linux-host port of apple-gfx-pci

## Summary

Upstream carries `hw/display/apple-gfx-pci.m` and
`hw/display/apple-gfx.m`, which drive Apple's
ParavirtualizedGraphics (PG) protocol from a macOS host via
the vendor-supplied `ParavirtualizedGraphics.framework`.
Linux QEMU hosts cannot use those files because the
framework has no Linux equivalent.

This series adds a companion Linux C port that speaks the
same PG protocol via `libapplegfx-vulkan` (a Mesa-lavapipe-
backed reimplementation of the PG framework's shell surface).
The guest-side `AppleParavirtGPU.kext` is unchanged; macOS
VMs running under KVM on Linux can now attach the same
paravirt GPU they attach on macOS hosts.

## What's new

  * `hw/display/apple-gfx-pci-linux.c` (~250 lines)
  * `hw/display/apple-gfx-common-linux.c` (~660 lines)
  * `hw/display/apple-gfx-linux.h` (~120 lines)
  * `hw/display/Kconfig` (1 new symbol)
  * `hw/display/meson.build` (1 new gated module)
  * `pc-bios/meson.build` (1 new blob entry)
  * `pc-bios/apple-gfx-pci.rom` (option ROM, development
    placeholder - see per-patch note)

No upstream file is functionally modified. The build gate is
additive; `libapplegfx-vulkan` is declared `required: false`
so the device drops out silently on hosts without the
library.

## Dependency

The `libapplegfx-vulkan` library is not yet packaged in any
distribution. See `LIBAPPLEGFX_DEPENDENCY.md` for the three
paths to unblock upstream merge (system package / subproject
/ inlined).

## Status

**Draft - do not merge.** Submission is blocked on:

  1. `libapplegfx-vulkan` distribution packaging or
     bundling.
  2. Replacing the placeholder `pc-bios/apple-gfx-pci.rom`
     blob with an in-tree EDK2 build.
  3. Review of the split by the apple-gfx.m author - the
     port is structured to avoid touching the existing
     Objective-C files, but a follow-up may want to factor
     the portable C logic out of `apple-gfx.m` into a
     shared `apple-gfx-common.c`.

## Testing

See `TESTING.md` for the end-to-end recipe.
