# Apple ParavirtualizedGraphics Linux Port — Phase 1.A

## Overview

This directory contains the Linux C port of QEMU's upstream `apple-gfx-pci.m` (157 lines) and `apple-gfx.m` (880 lines). The port targets Linux hosts and replaces the macOS-only Apple ParavirtualizedGraphics framework with libapplegfx-vulkan.

## Files

### apple-gfx-linux.h (87 lines)
**Local header** shared between the two .c files.
- **`AppleGFXLinuxState`** struct (replaces Obj-C `AppleGFXState`)
  - `PCIDevice parent_obj`
  - `MemoryRegion iomem_gfx` — MMIO register space
  - `lagfx_device_t *lagfx_dev` — libapplegfx device handle
  - `lagfx_display_t *lagfx_disp` — libapplegfx display handle
  - `QemuConsole *con`, `DisplaySurface *surface` — QEMU display integration
  - Task/memory management state (task_mutex, tasks queue)
  - Cursor, rendering, and mode-change state
- **Function declarations** for common init/realize and MMIO read/write
- Removed: Metal types (`MTLDevice`, `MTLTexture`, etc.), Obj-C block types, mach types

### apple-gfx-common-linux.c (423 lines)
**Portable logic ported from apple-gfx.m**. Contains:

#### Task/Memory Management (Phase 1.C stubs, ~80 LOC)
- `apple_gfx_create_task()` — shell callback, stub
- `apple_gfx_destroy_task()` — shell callback, stub
- `apple_gfx_map_memory()` — shell callback, stub
- `apple_gfx_unmap_memory()` — shell callback, stub
- `apple_gfx_read_memory()` — implemented; uses QEMU's `dma_memory_read()`

**Status:** Stubs awaiting Phase 1.C (memfd + mmap implementation).

#### Interrupt Delivery (~20 LOC)
- `apple_gfx_raise_interrupt()` — libapplegfx callback
- `apple_gfx_raise_interrupt_bh()` — BQL-safe MSI delivery via `msi_notify()`

**Status:** Complete; mirrors upstream logic.

#### Display Callbacks (~100 LOC)
Implemented as C function pointers (no Obj-C blocks):
- `apple_gfx_frame_ready()` — frame available; stub for Phase 1.B
- `apple_gfx_mode_changed()` — guest requests mode change
- `apple_gfx_cursor_glyph()` — cursor glyph update
- `apple_gfx_cursor_moved()` — cursor position change
- `apple_gfx_cursor_show()` — cursor visibility

**Status:** Most implemented; frame rendering (Phase 1.B) stubbed.

#### MMIO Read/Write (~10 LOC)
- `apple_gfx_mmio_read()` — delegates to `lagfx_mmio_read()`
- `apple_gfx_mmio_write()` — delegates to `lagfx_mmio_write()`

**Status:** Complete; straightforward wrappers.

#### Initialization (~130 LOC)
- `apple_gfx_common_init()` — device struct setup, task mutex init
- `apple_gfx_common_realize()` — creates libapplegfx device/display, sets up callbacks

**Status:** Core logic ported; error handling complete.

#### Display Mode Property (100+ LOC)
- `apple_gfx_get_display_mode()` / `apple_gfx_set_display_mode()` — QEMU property getters/setters
- Parses/emits `<width>x<height>@<refresh-rate>` format

**Status:** Complete; reuses QEMU property visitor pattern.

### apple-gfx-pci-linux.c (184 lines)
**PCI device variant**. Contains:

- `AppleGFXPCIState` struct (wraps `AppleGFXLinuxState`)
- `apple_gfx_pci_init()` — object initialization
- `apple_gfx_pci_realize()` — PCI BAR registration, MSI-X init, device creation
- `apple_gfx_pci_reset()` — reset via `lagfx_device_reset()`
- `apple_gfx_pci_unrealize()` — cleanup
- QEMU type registration (TypeInfo, type_init)

**Status:** Complete; mirrors upstream apple-gfx-pci.m structure.

## What's Ported

✓ **Portable QEMU/POSIX APIs** (80 calls): All QEMU PCI, display, DMA, aio, memory-region, and GLib APIs unchanged from upstream.

✓ **Interrupt delivery**: MSI-X via `msi_notify()`, BQL-safe BH scheduling.

✓ **Display initialization**: Graphic console, mode lists, property system.

✓ **Cursor management**: Glyph upload, position tracking (C function callbacks, not Obj-C blocks).

✓ **Memory task scaffolding**: Task queues, list management (actual VA remapping in Phase 1.C).

✓ **Device lifecycle**: Init, realize, reset, unrealize (no migration blocker cleanup yet).

## What's Stubbed (Phase 1.B / 1.C)

### Phase 1.C Tasks (Memory Management)

**`apple_gfx_create_task()`** — Needs memfd_create() + mmap(MAP_ANONYMOUS)
- Currently returns empty AppleGFXLinuxTask struct
- Must reserve a contiguous host VA range for guest-RAM remapping
- TODO: Implement with memfd or /dev/zero backing

**`apple_gfx_map_memory()`** — Needs mmap(MAP_FIXED) or mremap()
- Currently no-op; returns true
- Must map guest physical ranges into task VA space
- TODO: Implement mmap-based or mremap-based remapping (hardest part)

**`apple_gfx_unmap_memory()`** — Needs munmap() or mmap(MAP_FIXED) with zeros
- Currently no-op; returns true
- TODO: Implement zero-page replacement or munmap

### Phase 1.B Frame Rendering

**`apple_gfx_frame_ready_bh()`** — Needs Vulkan render pipeline
- Currently no-op stub
- Must call `lagfx_display_read_frame()` to get Vulkan-rendered pixels
- Must update QEMU DisplaySurface and trigger `dpy_gfx_update_full()`
- TODO: Implement frame read-out and surface update

**Cursor glyph deferred setup** — Placeholder BH
- `apple_gfx_cursor_glyph()` schedules deferred setup for BQL safety
- TODO: If needed, add dpy_cursor_define() call in BH

### Phase 0.B Feature (Option ROM)

**`apple_gfx_pci_init()`** — Option ROM handling
- TODO(Phase-0.B): Obtain ROM path / embed ROM as resource
- Comment indicates path: either sysroot, embedded resource, or skip
- On Linux, upstream macOS ROM is not available; may need custom or skip

## Design Differences from Upstream

| Aspect | Upstream (macOS) | Linux Port |
| --- | --- | --- |
| **Framework** | `#import <ParavirtualizedGraphics/...>` | `#include <libapplegfx-vulkan.h>` |
| **Device handle** | `id<PGDevice> pgdev` | `lagfx_device_t *lagfx_dev` |
| **Display handle** | `id<PGDisplay> pgdisp` | `lagfx_display_t *lagfx_disp` |
| **GPU API** | Metal (MTLDevice, MTLTexture, etc.) | Vulkan (via libapplegfx abstraction) |
| **Task management** | mach_vm_allocate/deallocate/remap | memfd + mmap (Phase 1.C) |
| **Dispatch queues** | dispatch_async, dispatch_get_global_queue | No async dispatch; BH + libapplegfx callbacks |
| **Callbacks** | Obj-C blocks (`^{...}`) | C function pointers |
| **Memory read** | Stub (PVG framework does it) | Uses `dma_memory_read()` |

## Dependencies

### libapplegfx-vulkan API Used

- **Device** — `lagfx_device_new()`, `lagfx_device_free()`, `lagfx_device_reset()`
- **Display** — `lagfx_display_new()`, `lagfx_display_free()`, `lagfx_display_cursor_position()`
- **MMIO** — `lagfx_mmio_read()`, `lagfx_mmio_write()`
- **Display read-out** — `lagfx_display_read_frame()` (Phase 1.B)
- **Callbacks** — Memory (create_task, destroy_task, map_memory, unmap_memory, read_memory) and display (frame_ready, mode_changed, cursor_glyph, cursor_moved, cursor_show)

### QEMU APIs

- PCI: `pci_register_bar()`, `msi_init()`, `msi_enabled()`, `msi_notify()`
- Display: `graphic_console_init()`, `dpy_gfx_update_full()`, `dpy_gfx_replace_surface()`, `dpy_mouse_set()`, `dpy_cursor_define()`, `graphic_hw_update_done()`
- Memory: `memory_region_init_io()`, `dma_memory_read()`, `address_space_translate()`
- AIO: `aio_bh_schedule_oneshot()`, `aio_wait_kick()`, `AIO_WAIT_WHILE()`
- Utilities: `qemu_create_displaysurface()`, `cursor_alloc()`, `cursor_unref()`, etc.

## Integration Notes

### Building

Not yet in meson.build. When integrated:

```meson
qemu_softmmu_sources += files(
  'hw/display/apple-gfx-pci-linux.c',
  'hw/display/apple-gfx-common-linux.c',
)

# Dependency on libapplegfx-vulkan
dependencies += dependency('libapplegfx-vulkan', required : false)
```

### Header Issues / Mismatches

**None identified yet.** The libapplegfx-vulkan.h C API closely matches the ported code's needs. Potential future issues:

- If libapplegfx needs more detailed texture descriptors (Metal → Vulkan impedance mismatch), Phase 1.B will reveal it.
- If task memory mapping requires additional shell callbacks (for IOMMU, etc.), Phase 1.C will reveal it.

## Testing / Validation

- **Phase 1.A validation**: File creation only; no compilation expected yet.
- **Phase 1.B validation**: Guest boots, displays BIOS/UEFI output (frame rendering).
- **Phase 1.C validation**: Guest RAM accessible to device (task memory mapping).
- **Full validation**: Guest macOS with 3D acceleration (GPU task scheduling, shader compilation).

## Code Metrics

| File | Lines | Status |
| --- | --- | --- |
| apple-gfx-linux.h | 87 | Complete |
| apple-gfx-common-linux.c | 423 | ~60% complete (Phase 1.B/1.C stubs) |
| apple-gfx-pci-linux.c | 184 | Complete |
| **Total** | **694** | **Phase 1.A done** |

Breakdown of the 423 LOC in common-linux.c:
- Task management (stubs): ~80 LOC
- Interrupt delivery: ~20 LOC
- Display callbacks: ~100 LOC
- MMIO read/write: ~10 LOC
- Device init/realize: ~130 LOC
- Display mode property: ~83 LOC

## Future Work

1. **Phase 1.B (Frame Rendering)**: Implement `apple_gfx_frame_ready_bh()` to read Vulkan frames via `lagfx_display_read_frame()`.
2. **Phase 1.C (Memory Mapping)**: Implement task VA management (memfd + mmap).
3. **Phase 0.B (Option ROM)**: Obtain or embed guest-side firmware.
4. **Optimization**: Consider async libapplegfx operations, GPU task batching, multi-display support.

