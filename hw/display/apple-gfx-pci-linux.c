/*
 * QEMU Apple ParavirtualizedGraphics.framework device — PCI variant, Linux C port
 *
 * Copyright © 2023–2026 Phil Dennis-Jordan / Amazon / QEMU contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Linux C port of apple-gfx-pci.m. Uses libapplegfx-vulkan for backend
 * instead of Apple's ParavirtualizedGraphics framework.
 *
 * RE annotations (paravirt-re/):
 *   - Cursor pipeline (stage 20%): cursor-rendering-stage-20.md
 *     Guest emits 0x13/0x14 on display vchan sub-channels (opcode 0x04).
 *   - Display vchan + opcode dispatch: PGFIFO-sub-channel-opcode-table.md,
 *     flows/display-init-flow.md, flows/display-swap-flow.md
 *   - PCI device identification: see paravirt-re/PROTOCOL.md §2.1,
 *     and AppleParavirtGPU.kext matching on IOPCIMatch "0xEEEE106B".
 *   - GPU cores property: gpu-cores-implementation-spec.md
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "hw/qdev-properties.h"
#include "apple-gfx-linux.h"
#include "trace.h"

/*
 * Upper bound on the `gpu_cores` property. Leaves headroom above Mesa
 * lavapipe's historical compile-time cap (LP_MAX_THREADS = 32) while
 * staying in sensible host territory; values above this clamp silently
 * to LAGFX_GPU_CORES_MAX with a warning. See
 * paravirt-re/gpu-cores-implementation-spec.md §1, §7.
 */
#define LAGFX_GPU_CORES_MAX 64u

/*
 * PCI device identification.
 *
 * x86_64 macOS Sequoia AppleParavirtGPU.kext matches on
 * IOPCIMatch = "0xEEEE106B". IOKit encodes this pair as
 *   (device_id << 16) | vendor_id
 * so the actual values to set on the PCI device are
 *   vendor_id = 0x106B, device_id = 0xEEEE.
 * Confirmed by the Phase -1.D.1 attach commit (qemu-mos15@c785645)
 * which produced a real AppleParavirtGPUControl::start() trace.
 */
#define PG_PCI_VENDOR_ID 0x106B
#define PG_PCI_DEVICE_ID 0xEEEE
#define PG_PCI_SUBSYSTEM_VENDOR_ID 0x106B
#define PG_PCI_SUBSYSTEM_ID 0xEEEE
#define PG_PCI_MAX_MSI_VECTORS 32
#define PG_PCI_BAR_MMIO 0

/*
 * Default option ROM filename, installed into qemu_datadir (pc-bios/) by the
 * build system. QEMU's PCI core looks this up via qemu_find_file() at realize
 * time and maps it into a ROM BAR for us. Users can override at the CLI with
 * `-device apple-gfx-pci,romfile=/path/to/other.rom` — the `romfile` property
 * is inherited from the base PCIDevice class (see hw/pci/pci.c).
 *
 * Phase 1.E: Apple's AppleParavirtEFI.rom is shipped during dev; Phase 5.X
 * replaces with own EDK2 build.
 */
#define PG_PCI_DEFAULT_ROMFILE "apple-gfx-pci.rom"

#define TYPE_APPLE_GFX_PCI "apple-gfx-pci"

OBJECT_DECLARE_SIMPLE_TYPE(AppleGFXPCIState, APPLE_GFX_PCI)

struct AppleGFXPCIState {
    AppleGFXLinuxState common;
};

/* Forward declare from apple-gfx-common-linux.c */
extern const PropertyInfo qdev_prop_apple_gfx_display_mode;

static void
apple_gfx_pci_init(Object *obj)
{
    AppleGFXPCIState *s = APPLE_GFX_PCI(obj);

    /*
     * Option ROM loading is driven by PCIDeviceClass::romfile (set in
     * class_init below). QEMU's PCI core handles the BAR allocation and
     * file lookup during apple_gfx_pci_realize() / pci_qdev_realize().
     * No per-instance setup is needed here.
     */

    apple_gfx_common_init(obj, &s->common);
}

static void
apple_gfx_pci_realize(PCIDevice *pci_dev, Error **errp)
{
    AppleGFXPCIState *s = APPLE_GFX_PCI(pci_dev);
    AppleGFXLinuxState *common = &s->common;
    lagfx_device_descriptor_t device_desc;
    int ret;

    /* Register MMIO BAR */
    pci_register_bar(pci_dev, PG_PCI_BAR_MMIO,
                     PCI_BASE_ADDRESS_SPACE_MEMORY, &common->iomem_gfx);

    /* Initialize MSI-X for interrupt delivery */
    ret = msi_init(pci_dev, 0x0 /* config offset; 0 = auto */,
                   PG_PCI_MAX_MSI_VECTORS, true /* msi64bit */,
                   false /* msi_per_vector_mask */, errp);
    if (ret != 0) {
        return;
    }

    /* Prepare device descriptor for libapplegfx */
    memset(&device_desc, 0, sizeof(device_desc));

    /* Shell callbacks (memory + interrupt) */
    device_desc.shell.opaque = common;
    device_desc.shell.create_task = apple_gfx_create_task;
    device_desc.shell.destroy_task = apple_gfx_destroy_task;
    device_desc.shell.map_memory = apple_gfx_map_memory;
    device_desc.shell.unmap_memory = apple_gfx_unmap_memory;
    device_desc.shell.read_memory = apple_gfx_read_memory;
    device_desc.shell.write_memory = apple_gfx_write_memory;
    device_desc.shell.raise_interrupt = apple_gfx_raise_interrupt;

    /* MMIO region size hint (0 = use library default) */
    device_desc.mmio_region_size = 0;

    /* No pre-existing Vulkan instance */
    device_desc.shell_vulkan_instance = NULL;

    /*
     * Plumb `gpu_cores` -> descriptor thread_count -> LP_NUM_THREADS.
     *
     * Clamp >64 with a warning. 0 is the "unset; lavapipe default"
     * sentinel and passes straight through. No hard error on
     * over-range; operators may set high values intentionally (host
     * CPU count, cgroup limit). See
     * paravirt-re/gpu-cores-implementation-spec.md §7.
     */
    if (common->gpu_cores > LAGFX_GPU_CORES_MAX) {
        warn_report("apple-gfx-pci: gpu_cores=%u exceeds max %u; "
                    "clamping (lavapipe may clamp further)",
                    common->gpu_cores, LAGFX_GPU_CORES_MAX);
        common->gpu_cores = LAGFX_GPU_CORES_MAX;
    }
    device_desc.thread_count = common->gpu_cores;

/*
 * Initialize common device state — this is where the display
 * callbacks (frame_ready, mode_changed, cursor_glyph, cursor_moved,
 * cursor_show) are registered with libapplegfx-vulkan.
 *
 * Cursor pipeline (RE-confirmed at paravirt-re/cursor-rendering-stage-20.md):
 *   Guest emits 0x13 CmdDisplayCursorShow →
 *     libapplegfx handler → cursor_moved + cursor_show callbacks →
 *     QEMU dpy_mouse_set() (cursor position + visibility)
 *
 *   Guest emits 0x14 CmdDisplayCursorGlyph →
 *     libapplegfx handler → cursor_glyph callback →
 *     QEMU dpy_cursor_define() (cursor image upload, BGRA→RGBA)
 *
 * Both opcodes travel on display vchan sub-channels (not main
 * display channel) — created via opcode 0x04 CmdDefineChildFIFO.
 * See paravirt-re/flows/display-init-flow.md and
 * paravirt-re/AppleParavirtDisplayPipe-vtable-decoded.md.
 *
 * Stage 20% target: cursor visible + movable. Stage 30%+ builds
 * on this with actual rendering via render opcodes.
 *
 * Cursor visibility status (2026-04-30):
 *   - Callbacks NOW FIRE in QEMU (commit 0511f24 wires up
 *     cursor opcode dispatch via protocol.c fix).
 *   - s->cursor_show flag is set by apple_gfx_cursor_show().
 *   - dpy_mouse_set() is called with s->cursor_show as visibility.
 *   - DEADLOCK BLOCKER: WindowServer hangs in ABBA deadlock,
 *     so cursor may not be visible in practice (WS never completes
 *     display initialization to show cursor).
 *   - See paravirt-re/library/stage-20-cursor-status.md.
 *
 * What's needed for noVNC to show cursor:
 *   1. Deadlock must be resolved (stage 10% blocker).
 *   2. VNC server must pick up cursor changes via QEMU's
 *      dpy_cursor_define() + dpy_mouse_set() callbacks.
 *   3. For hardware cursor: QEMU's VNC uses cursor data from
 *      dpy_cursor_define(); ensure BGRA→RGBA conversion is correct.
 *   4. For software cursor (fallback): guest renders cursor into
 *      framebuffer; requires working render opcodes (stage 30%+).
 *   5. noVNC cursor visibility also depends on VNC encoding
 *      (rich cursor encoding via QEMU's cursor data path).
 */
    if (!apple_gfx_common_realize(common, DEVICE(pci_dev), &device_desc, errp)) {
        return;
    }

    trace_apple_gfx_pci_realize();
}

static void
apple_gfx_pci_reset(Object *obj, ResetType type)
{
    AppleGFXPCIState *s = APPLE_GFX_PCI(obj);

    if (s->common.lagfx_dev) {
        lagfx_device_reset(s->common.lagfx_dev);
    }

    trace_apple_gfx_pci_reset();
}

static void
apple_gfx_pci_unrealize(DeviceState *dev)
{
    AppleGFXPCIState *s = APPLE_GFX_PCI(dev);

    if (s->common.lagfx_disp) {
        lagfx_display_free(s->common.lagfx_disp);
        s->common.lagfx_disp = NULL;
    }

    if (s->common.lagfx_dev) {
        lagfx_device_free(s->common.lagfx_dev);
        s->common.lagfx_dev = NULL;
    }

    if (s->common.surface) {
        qemu_free_displaysurface(s->common.surface);
        s->common.surface = NULL;
    }

    if (s->common.cursor) {
        cursor_unref(s->common.cursor);
        s->common.cursor = NULL;
    }
}

static const Property apple_gfx_pci_properties[] = {
    DEFINE_PROP_ARRAY("display-modes", AppleGFXPCIState,
                      common.num_display_modes, common.display_modes,
                      qdev_prop_apple_gfx_display_mode, AppleGFXDisplayMode),
    /*
     * Per-VM lavapipe worker-thread budget. 0 = let lavapipe pick its
     * default (host core count); N > 0 sets LP_NUM_THREADS=N before
     * Vulkan init. Accepted values 0..LAGFX_GPU_CORES_MAX; higher
     * values are clamped at realize with a warning. Reset-only
     * (LP_NUM_THREADS is read by Mesa once, at Vulkan-ICD init).
     * See paravirt-re/gpu-cores-implementation-spec.md.
     */
    DEFINE_PROP_UINT32("gpu_cores", AppleGFXPCIState, common.gpu_cores, 0),
};

static void
apple_gfx_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pci = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = apple_gfx_pci_reset;
    dc->desc = "Apple Paravirtualized Graphics PCI Display Controller (Linux)";
    dc->hotpluggable = false;
    dc->unrealize = apple_gfx_pci_unrealize;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);

    pci->vendor_id = PG_PCI_VENDOR_ID;
    pci->device_id = PG_PCI_DEVICE_ID;
    pci->subsystem_vendor_id = PG_PCI_SUBSYSTEM_VENDOR_ID;
    pci->subsystem_id = PG_PCI_SUBSYSTEM_ID;
    pci->class_id = PCI_CLASS_DISPLAY_VGA;
    pci->realize = apple_gfx_pci_realize;

    /*
     * Phase 1.E: Apple's AppleParavirtEFI.rom is shipped during dev;
     * Phase 5.X replaces with own EDK2 build.
     *
     * This sets the default ROM. It can be overridden at instantiation via
     *   -device apple-gfx-pci,romfile=/path/to/alt.rom
     * The `romfile` property is registered on the base PCIDevice class
     * (see DEFINE_PROP_STRING("romfile", ...) in hw/pci/pci.c), and the PCI
     * core loads it via qemu_find_file(QEMU_FILE_TYPE_BIOS, ...) at realize.
     */
    pci->romfile = PG_PCI_DEFAULT_ROMFILE;

    device_class_set_props(dc, apple_gfx_pci_properties);
}

static const TypeInfo apple_gfx_pci_type = {
    .name = TYPE_APPLE_GFX_PCI,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(AppleGFXPCIState),
    .class_init = apple_gfx_pci_class_init,
    .instance_init = apple_gfx_pci_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { },
    },
};

static void
apple_gfx_pci_register_types(void)
{
    type_register_static(&apple_gfx_pci_type);
}

type_init(apple_gfx_pci_register_types)
