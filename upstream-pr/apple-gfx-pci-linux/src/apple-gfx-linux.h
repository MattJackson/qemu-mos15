/*
 * QEMU Apple ParavirtualizedGraphics.framework device — Linux C variant
 * Local header for apple-gfx-pci-linux.c and apple-gfx-common-linux.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_APPLE_GFX_LINUX_H
#define QEMU_APPLE_GFX_LINUX_H

#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "ui/console.h"
#include "system/memory.h"
#include <libapplegfx-vulkan.h>

/* Task-memory management: Linux variant.
 *
 * Each task owns a reserved VA range backed by libapplegfx-vulkan's
 * memfd-based lagfx_task. When map_memory is invoked, we translate each
 * guest-physical range to a host pointer via address_space_translate(),
 * then hand that pointer to lagfx_task_map_host_memory(). We ref the
 * backing MemoryRegion to keep it alive for the mapping's lifetime.
 */
typedef struct AppleGFXLinuxTask {
    QTAILQ_ENTRY(AppleGFXLinuxTask) node;
    lagfx_task_t *lagfx_task;  /* opaque handle from lagfx_task_create */
    void *base_address;        /* reserved VA base, from lagfx_task_create */
    uint64_t size;             /* reserved VA range length */
    GPtrArray *mapped_regions; /* MemoryRegions ref'd for this task */
} AppleGFXLinuxTask;

typedef QTAILQ_HEAD(, AppleGFXLinuxTask) AppleGFXLinuxTaskList;

/* Display mode */
typedef struct {
    uint16_t width_px;
    uint16_t height_px;
    uint16_t refresh_rate_hz;
} AppleGFXDisplayMode;

/* Main device state — replaces Obj-C AppleGFXState */
typedef struct AppleGFXLinuxState {
    PCIDevice parent_obj;

    /* MMIO register space */
    MemoryRegion iomem_gfx;

    /* libapplegfx-vulkan handles */
    lagfx_device_t *lagfx_dev;
    lagfx_display_t *lagfx_disp;

    /* QEMU display integration */
    QemuConsole *con;
    DisplaySurface *surface;

    /* Task/memory management (protected by task_mutex) */
    QemuMutex task_mutex;
    AppleGFXLinuxTaskList tasks;

    /* Cursor and mode state (BQL protected) */
    QEMUCursor *cursor;
    bool cursor_show;

    /* Display modes (from device properties) */
    AppleGFXDisplayMode *display_modes;
    uint32_t num_display_modes;

    /*
     * Per-VM lavapipe worker-thread budget (from -device gpu_cores=N).
     * 0 = unset; let lavapipe pick its default (host core count).
     * Copied into lagfx_device_descriptor_t::thread_count at realize
     * time, where the library translates it into LP_NUM_THREADS before
     * the first Vulkan call. Process-global: if multiple apple-gfx-pci
     * devices are created in one QEMU process (an atypical deploy; the
     * single-VM-per-process case is the tested path), the first
     * non-zero value wins. Reset-only; changes after QEMU start are
     * not picked up.
     */
    uint32_t gpu_cores;

    /* Frame rendering state */
    int8_t pending_frames;
    bool gfx_update_requested;
    bool new_frame_ready;
    uint32_t rendering_frame_width;
    uint32_t rendering_frame_height;

    /* Reference to option ROM path (unused on Linux: the PCI core loads the
     * ROM directly via PCIDeviceClass::romfile; see apple-gfx-pci-linux.c). */
    const char *option_rom_path;
} AppleGFXLinuxState;

/* Function declarations shared between -pci-linux.c and -common-linux.c */

void apple_gfx_common_init(Object *obj, AppleGFXLinuxState *s);
bool apple_gfx_common_realize(AppleGFXLinuxState *s, DeviceState *dev,
                              lagfx_device_descriptor_t *desc, Error **errp);

/* Memory-region MemoryRegionOps handlers (common) */
uint64_t apple_gfx_mmio_read(void *opaque, hwaddr offset, unsigned size);
void apple_gfx_mmio_write(void *opaque, hwaddr offset, uint64_t val, unsigned size);

/* libapplegfx shell callbacks implemented in apple-gfx-common-linux.c.
 * These are populated into lagfx_device_descriptor_t::shell by the PCI (and
 * future MMIO) transport variants, so their addresses must be visible across
 * translation units — hence extern here instead of file-local static. */
lagfx_task_t *apple_gfx_create_task(void *opaque, uint64_t vm_size,
                                    void **base_address_out);
void apple_gfx_destroy_task(void *opaque, lagfx_task_t *task);
bool apple_gfx_map_memory(void *opaque, lagfx_task_t *task,
                          uint64_t virtual_offset,
                          const lagfx_physical_range_t *ranges,
                          size_t range_count, bool read_only);
bool apple_gfx_unmap_memory(void *opaque, lagfx_task_t *task,
                            uint64_t virtual_offset, uint64_t length);
bool apple_gfx_read_memory(void *opaque, uint64_t guest_physical_address,
                           uint64_t length, void *dst);
void apple_gfx_raise_interrupt(void *opaque, uint32_t vector);

#endif /* QEMU_APPLE_GFX_LINUX_H */
