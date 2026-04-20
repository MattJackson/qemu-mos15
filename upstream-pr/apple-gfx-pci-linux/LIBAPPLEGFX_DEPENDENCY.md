# The libapplegfx-vulkan dependency

This patch series cannot land in upstream QEMU until the
`libapplegfx-vulkan` dependency is either:

1. **Accepted as a distribution-shipped system library**
   (Debian / Fedora / Alpine / Arch / Gentoo), or
2. **Bundled as a submodule under `subprojects/`**, or
3. **Linked as an external mesonproject dependency**
   (`meson.build`-visible subproject fetched at configure
   time) with an upstream home that QEMU maintainers trust.

The library reimplements the PGDevice / PGShellCallbacks
subset of Apple's ParavirtualizedGraphics.framework on top
of Mesa lavapipe. It is currently a single-maintainer project
and has not been packaged for any distribution.

## What the library provides

Headers and symbols (public API):

  * `lagfx_task_t`, `lagfx_task_create`, `lagfx_task_destroy`,
    `lagfx_task_map_host_memory`, `lagfx_task_unmap`
  * `lagfx_device_t`, `lagfx_device_new`, `lagfx_device_free`,
    `lagfx_device_reset`
  * `lagfx_display_t`, `lagfx_display_new`, `lagfx_display_free`,
    `lagfx_display_cursor_position`
  * `lagfx_mmio_read` / `lagfx_mmio_write`
  * `lagfx_physical_range_t`, `lagfx_coord_t` typedefs
  * `lagfx_device_descriptor_t`, `lagfx_display_descriptor_t`
    with shell-callback function-pointer tables

All of these appear in the patch-5 realize path. The
guest-side protocol (MMIO register layout, IRQ semantics) is
authored inside the library so the QEMU side can stay
transport-agnostic.

## What upstream needs to decide

Three feasible shapes to merge:

### Option A - system package

`libapplegfx-vulkan` becomes a packaged system library in at
least one major distribution, and QEMU's
`hw/display/meson.build` uses `dependency('libapplegfx-vulkan',
required: false)` (as currently in the patch). Builds with
the package installed get the device; builds without skip it
silently. Zero new code in QEMU, matches how `virgl` /
`rutabaga` are handled.

**Prereq:** distro packaging. Blocker.

### Option B - subproject submodule

Add `libapplegfx-vulkan` as a git-submodule under
`subprojects/` with a wrap file, similar to how some meson
projects ship optional dependencies. Meson's subproject
machinery handles both "system library found" and "submodule
bundled" cases transparently.

**Prereq:** library must have a stable ABI and a QEMU-
acceptable license (GPL-2.0-or-later is fine; the current
library is MIT).

### Option C - inlined

Fold the library source into QEMU's tree as
`hw/display/apple-gfx-linux-lib/` and build it as part of
the device module. Heavier patch set but removes the external
dependency entirely.

**Prereq:** license compatibility (MIT-to-GPL-2.0 is fine);
library maintainer consent to vendor into QEMU.

## Recommendation

Pursue **Option A** first (the library maintainer is me and
I am willing to package it). Fall back to Option B if
distros are slow; Option C if the library stays small enough
that vendoring is reasonable.

## Status check

As of this submission the library is:

  * Developed in its own repository under MIT license.
  * Not packaged in any distribution.
  * ABI-stable at the API surface this patch series uses, but
    the library is pre-1.0.

Upstream submission is therefore **blocked on Option A / B**.
