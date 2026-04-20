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

/* Task-memory management: Linux variant (stub; actual impl in Phase 1.C) */
typedef struct AppleGFXLinuxTask {
    QTAILQ_ENTRY(AppleGFXLinuxTask) node;
    lagfx_task_t *lagfx_task;  /* opaque handle to libapplegfx task */
    void *base_address;        /* reserved VA base */
    uint64_t size;
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

    /* Frame rendering state */
    int8_t pending_frames;
    bool gfx_update_requested;
    bool new_frame_ready;
    uint32_t rendering_frame_width;
    uint32_t rendering_frame_height;

    /* Reference to option ROM path (macOS feature; stubbed on Linux) */
    const char *option_rom_path;
} AppleGFXLinuxState;

/* Function declarations shared between -pci-linux.c and -common-linux.c */

void apple_gfx_common_init(Object *obj, AppleGFXLinuxState *s);
bool apple_gfx_common_realize(AppleGFXLinuxState *s, DeviceState *dev,
                              lagfx_device_descriptor_t *desc, Error **errp);

/* Memory-region MemoryRegionOps handlers (common) */
uint64_t apple_gfx_mmio_read(void *opaque, hwaddr offset, unsigned size);
void apple_gfx_mmio_write(void *opaque, hwaddr offset, uint64_t val, unsigned size);

#endif /* QEMU_APPLE_GFX_LINUX_H */
