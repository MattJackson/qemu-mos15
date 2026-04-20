/*
 * QEMU Apple ParavirtualizedGraphics.framework device — PCI variant, Linux C port
 *
 * Copyright © 2023–2026 Phil Dennis-Jordan / Amazon / QEMU contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Linux C port of apple-gfx-pci.m. Uses libapplegfx-vulkan for backend
 * instead of Apple's ParavirtualizedGraphics framework.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
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

/* PCI device identification */
#define PG_PCI_VENDOR_ID 0x106b  /* Apple Inc. */
#define PG_PCI_DEVICE_ID 0x1b30  /* ParavirtualizedGraphics */
#define PG_PCI_MAX_MSI_VECTORS 64
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

    /* Initialize common device state */
    if (!apple_gfx_common_realize(common, DEVICE(pci_dev), &device_desc, errp)) {
        return;
    }

    /*
     * Upstream hw/display/trace-events does not define apple_gfx_pci_realize
     * (the Darwin port inlined this into apple_gfx_common_init tracing).
     * Rather than carry a full trace-events overlay for two PCI-only trace
     * points, log via qemu_log_mask(LOG_TRACE). See M1 dry-run audit.
     */
    qemu_log_mask(LOG_TRACE, "apple-gfx-pci: realize\n");
}

static void
apple_gfx_pci_reset(Object *obj, ResetType type)
{
    AppleGFXPCIState *s = APPLE_GFX_PCI(obj);

    if (s->common.lagfx_dev) {
        lagfx_device_reset(s->common.lagfx_dev);
    }

    qemu_log_mask(LOG_TRACE, "apple-gfx-pci: reset\n");
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
    DEFINE_PROP_END_OF_LIST(),
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
    pci->class_id = PCI_CLASS_DISPLAY_OTHER;
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
