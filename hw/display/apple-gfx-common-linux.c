/*
 * QEMU Apple ParavirtualizedGraphics.framework device — Linux C port
 * Common device logic, portable from apple-gfx.m
 *
 * Copyright © 2023–2026 Amazon/QEMU contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file contains the portable parts of apple-gfx.m refactored to C
 * and ported to use libapplegfx-vulkan instead of Apple's framework.
 * Ported for Linux; Metal/Metal-specific code removed.
 */

#include "qemu/osdep.h"
#include "qemu/lockable.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qapi/visitor.h"
#include "qapi/error.h"
#include "block/aio-wait.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "migration/blocker.h"
#include "ui/console.h"
#include "apple-gfx-linux.h"
#include "trace.h"

#define PG_PCI_BAR_MMIO 0
#define PG_PCI_VENDOR_ID 0x106b  /* Apple Inc. */
#define PG_PCI_DEVICE_ID 0x1b30  /* ParavirtualizedGraphics */
#define PG_PCI_MAX_MSI_VECTORS 64

static const AppleGFXDisplayMode apple_gfx_default_modes[] = {
    { 1920, 1080, 60 },
    { 1440, 1080, 60 },
    { 1280, 1024, 60 },
};

static Error *apple_gfx_mig_blocker;
static uint32_t next_pgdisplay_serial_num = 1;

/* ------ Memory Task Management ------ */

/*
 * Translate a guest-physical range to a host pointer for direct access.
 *
 * Returns NULL if the range crosses a non-RAM region, if the region
 * cannot be accessed directly (e.g. MMIO, ROM with writes, coalesced),
 * or if the translation is shorter than requested.
 *
 * On success *mapping_in_region is set to the backing MemoryRegion; the
 * caller must memory_region_ref() it if retaining the pointer beyond the
 * current RCU critical section.
 *
 * Semantically equivalent to apple_gfx_host_ptr_for_gpa_range() in the
 * upstream Darwin/mach port (hw/display/apple-gfx.m).
 */
static void *
apple_gfx_host_ptr_for_gpa_range(uint64_t guest_physical,
                                 uint64_t length, bool read_only,
                                 MemoryRegion **mapping_in_region)
{
    MemoryRegion *ram_region;
    char *host_ptr;
    hwaddr ram_region_offset = 0;
    hwaddr ram_region_length = length;

    ram_region = address_space_translate(&address_space_memory,
                                         guest_physical,
                                         &ram_region_offset,
                                         &ram_region_length, !read_only,
                                         MEMTXATTRS_UNSPECIFIED);

    if (!ram_region || ram_region_length < length ||
        !memory_access_is_direct(ram_region, !read_only,
                                 MEMTXATTRS_UNSPECIFIED)) {
        return NULL;
    }

    host_ptr = memory_region_get_ram_ptr(ram_region);
    if (!host_ptr) {
        return NULL;
    }
    host_ptr += ram_region_offset;
    *mapping_in_region = ram_region;
    return host_ptr;
}

/*
 * Shell callback: create a reserved virtual address range.
 *
 * Wraps lagfx_task_create(), which performs the underlying
 * mmap(PROT_NONE) reservation (see libapplegfx-vulkan/src/memory/task.c).
 * Returns the opaque lagfx_task_t handle — libapplegfx passes this back
 * to us in later map/unmap/destroy calls so we can find the matching
 * AppleGFXLinuxTask bookkeeping entry.
 *
 * On allocation failure returns NULL; libapplegfx treats this as
 * "task creation failed" and propagates the error through its own
 * control paths. We cannot raise a QEMU Error** here because the
 * shell-callback signature is error-code-only.
 */
static lagfx_task_t *
apple_gfx_create_task(void *opaque, uint64_t vm_size, void **base_address_out)
{
    AppleGFXLinuxState *s = opaque;
    AppleGFXLinuxTask *task;
    lagfx_task_t *lagfx_task;
    void *base_addr = NULL;

    /* Reserve the VA range via libapplegfx-vulkan's memfd-backed helper. */
    lagfx_task = lagfx_task_create((size_t)vm_size, &base_addr);
    if (!lagfx_task) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "apple-gfx: lagfx_task_create(vm_size=0x%" PRIx64
                      ") failed\n", vm_size);
        *base_address_out = NULL;
        return NULL;
    }

    task = g_new0(AppleGFXLinuxTask, 1);
    task->lagfx_task = lagfx_task;
    task->base_address = base_addr;
    task->size = vm_size;
    task->mapped_regions = g_ptr_array_sized_new(2 /* usually enough */);

    QEMU_LOCK_GUARD(&s->task_mutex);
    QTAILQ_INSERT_TAIL(&s->tasks, task, node);

    *base_address_out = base_addr;
    trace_apple_gfx_create_task(vm_size, base_addr);

    return lagfx_task;
}

static void
apple_gfx_destroy_task(void *opaque, lagfx_task_t *task)
{
    AppleGFXLinuxState *s = opaque;
    AppleGFXLinuxTask *linux_task = NULL;
    AppleGFXLinuxTask *it;

    WITH_QEMU_LOCK_GUARD(&s->task_mutex) {
        QTAILQ_FOREACH(it, &s->tasks, node) {
            if (it->lagfx_task == task) {
                linux_task = it;
                QTAILQ_REMOVE(&s->tasks, linux_task, node);
                break;
            }
        }
    }

    if (!linux_task) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "apple-gfx: destroy_task for unknown handle %p\n", task);
        return;
    }

    trace_apple_gfx_destroy_task(task, linux_task->mapped_regions
                                 ? linux_task->mapped_regions->len : 0);

    /* Drop MemoryRegion references taken during map_memory. */
    if (linux_task->mapped_regions) {
        for (guint i = 0; i < linux_task->mapped_regions->len; ++i) {
            MemoryRegion *region = g_ptr_array_index(linux_task->mapped_regions, i);
            memory_region_unref(region);
        }
        g_ptr_array_unref(linux_task->mapped_regions);
    }

    /* Release the VA reservation + memfd backing. */
    lagfx_task_destroy(task);

    g_free(linux_task);
}

/*
 * Shell callback: map one or more guest-physical ranges into the task's
 * reserved VA range, starting at virtual_offset. Matching Apple's
 * mapMemory contract, ranges are packed contiguously: virtual_offset
 * advances by each range's length as we walk the list.
 *
 * For each range we translate GPA -> host pointer (via QEMU's address-
 * space machinery), take a ref on the backing MemoryRegion, and hand
 * the host pointer to lagfx_task_map_host_memory(). The library handles
 * the memfd + mmap(MAP_FIXED) mechanics internally.
 *
 * Note: the current library implementation copies the host-addr contents
 * into the memfd on map; a future optimisation may share pages directly
 * (see libapplegfx-vulkan/src/memory/task.c). From this shell's view the
 * contract is "after this returns true, guest-visible writes to GPA are
 * reflected in the task VA range" — verified only for read-mostly ranges
 * in Phase 1.A.1. Flag for 1.B if we observe guest-writable DMA regions
 * relying on post-map coherence. (See R3 in phase-1a2-decoder-plan.md.)
 */
static bool
apple_gfx_map_memory(void *opaque, lagfx_task_t *task,
                     uint64_t virtual_offset,
                     const lagfx_physical_range_t *ranges,
                     size_t range_count, bool read_only)
{
    AppleGFXLinuxState *s = opaque;
    AppleGFXLinuxTask *linux_task = NULL;
    AppleGFXLinuxTask *it;
    bool success = true;

    trace_apple_gfx_map_memory(task, range_count, virtual_offset, read_only);

    RCU_READ_LOCK_GUARD();
    QEMU_LOCK_GUARD(&s->task_mutex);

    QTAILQ_FOREACH(it, &s->tasks, node) {
        if (it->lagfx_task == task) {
            linux_task = it;
            break;
        }
    }
    if (!linux_task) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "apple-gfx: map_memory for unknown task handle %p\n",
                      task);
        return false;
    }

    for (size_t i = 0; i < range_count; i++) {
        const lagfx_physical_range_t *range = &ranges[i];
        MemoryRegion *region = NULL;
        void *host_ptr;

        trace_apple_gfx_map_memory_range(i, range->guest_physical_address,
                                         range->length);

        host_ptr = apple_gfx_host_ptr_for_gpa_range(range->guest_physical_address,
                                                    range->length, read_only,
                                                    &region);
        if (!host_ptr) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "apple-gfx: map_memory: GPA 0x%" PRIx64
                          " len 0x%" PRIx64 " not directly accessible\n",
                          range->guest_physical_address, range->length);
            success = false;
            virtual_offset += range->length;
            continue;
        }

        if (!g_ptr_array_find(linux_task->mapped_regions, region, NULL)) {
            g_ptr_array_add(linux_task->mapped_regions, region);
            memory_region_ref(region);
        }

        if (!lagfx_task_map_host_memory(task, virtual_offset, host_ptr,
                                         range->length, read_only)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "apple-gfx: lagfx_task_map_host_memory failed"
                          " at task_offset=0x%" PRIx64 " len=0x%" PRIx64 "\n",
                          virtual_offset, range->length);
            success = false;
        }

        virtual_offset += range->length;
    }

    return success;
}

static bool
apple_gfx_unmap_memory(void *opaque, lagfx_task_t *task,
                       uint64_t virtual_offset, uint64_t length)
{
    trace_apple_gfx_unmap_memory(task, virtual_offset, length);

    if (!lagfx_task_unmap(task, virtual_offset, length)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "apple-gfx: lagfx_task_unmap failed"
                      " at offset=0x%" PRIx64 " len=0x%" PRIx64 "\n",
                      virtual_offset, length);
        return false;
    }
    return true;
}

static bool
apple_gfx_read_memory(void *opaque, uint64_t guest_physical_address,
                      uint64_t length, void *dst)
{
    AppleGFXLinuxState *s = opaque;
    MemTxResult r;

    trace_apple_gfx_read_memory(guest_physical_address, length, dst);

    /* Use QEMU's DMA engine to read guest RAM into dst */
    r = dma_memory_read(&address_space_memory, guest_physical_address,
                        dst, length, MEMTXATTRS_UNSPECIFIED);

    return (r == MEMTX_OK);
}

/* ------ Interrupt Injection ------ */

typedef struct {
    PCIDevice *device;
    uint32_t vector;
} AppleGFXInterruptJob;

static void
apple_gfx_raise_interrupt_bh(void *opaque)
{
    AppleGFXInterruptJob *job = opaque;

    if (msi_enabled(job->device)) {
        msi_notify(job->device, job->vector);
    }
    g_free(job);
}

static void
apple_gfx_raise_interrupt(void *opaque, uint32_t vector)
{
    AppleGFXInterruptJob *job;
    /* opaque is the AppleGFXLinuxState; extract PCIDevice parent */
    AppleGFXLinuxState *s = opaque;
    PCIDevice *pci_dev = &s->parent_obj;

    trace_apple_gfx_raise_irq(vector);

    job = g_malloc0(sizeof(*job));
    job->device = pci_dev;
    job->vector = vector;

    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            apple_gfx_raise_interrupt_bh, job);
}

/* ------ Display Callbacks ------ */

static void
apple_gfx_frame_ready_bh(void *opaque)
{
    AppleGFXLinuxState *s = opaque;

    /* Drop frames if guest gets too far ahead */
    if (s->pending_frames >= 2) {
        return;
    }
    ++s->pending_frames;
    if (s->pending_frames > 1) {
        return;
    }

    /* TODO(Phase-1.B): Trigger Vulkan render + read-out via lagfx_display_read_frame */
}

static void
apple_gfx_frame_ready(void *opaque)
{
    trace_apple_gfx_new_frame();
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            apple_gfx_frame_ready_bh, opaque);
}

static void
apple_gfx_mode_changed(void *opaque, uint32_t width_px, uint32_t height_px)
{
    AppleGFXLinuxState *s = opaque;

    trace_apple_gfx_mode_change(width_px, height_px);

    BQL_LOCK_GUARD();

    if (s->surface &&
        width_px == surface_width(s->surface) &&
        height_px == surface_height(s->surface)) {
        return;
    }

    s->surface = qemu_create_displaysurface(width_px, height_px);
    dpy_gfx_replace_surface(s->con, s->surface);
}

static void
apple_gfx_cursor_glyph_bh(void *opaque)
{
    /* Placeholder: cursor glyph setup deferred to BH for BQL safety */
    /* TODO(Phase-1.B): Call dpy_cursor_define with the glyph data */
    g_free(opaque);
}

static void
apple_gfx_cursor_glyph(void *opaque,
                        const uint8_t *bgra_pixels,
                        uint32_t width, uint32_t height,
                        lagfx_coord_t hotspot)
{
    AppleGFXLinuxState *s = opaque;

    trace_apple_gfx_cursor_set(32, width, height);

    if (s->cursor) {
        cursor_unref(s->cursor);
        s->cursor = NULL;
    }

    s->cursor = cursor_alloc(width, height);
    s->cursor->hot_x = hotspot.x;
    s->cursor->hot_y = hotspot.y;

    uint32_t *dest_px = s->cursor->data;
    const uint8_t *src_px = bgra_pixels;

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            /* bgra_pixels is BGRA; QEMU cursor expects RGBA */
            *dest_px = (src_px[2] << 16u) |  /* R */
                       (src_px[1] <<  8u) |  /* G */
                       (src_px[0] <<  0u) |  /* B */
                       (src_px[3] << 24u);   /* A */
            ++dest_px;
            src_px += 4;
        }
    }

    dpy_cursor_define(s->con, s->cursor);

    /* Update cursor position on next BH */
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            apple_gfx_cursor_glyph_bh, NULL);
}

static void
apple_gfx_cursor_moved_bh(void *opaque)
{
    AppleGFXLinuxState *s = opaque;
    lagfx_coord_t pos = lagfx_display_cursor_position(s->lagfx_disp);

    dpy_mouse_set(s->con, pos.x, pos.y, s->cursor_show);
}

static void
apple_gfx_cursor_moved(void *opaque)
{
    trace_apple_gfx_cursor_move();
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            apple_gfx_cursor_moved_bh, opaque);
}

static void
apple_gfx_cursor_show(void *opaque, bool show)
{
    AppleGFXLinuxState *s = opaque;

    trace_apple_gfx_cursor_show(show);
    s->cursor_show = show;

    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                            apple_gfx_cursor_moved_bh, opaque);
}

/* ------ MMIO Read/Write ------ */

uint64_t
apple_gfx_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
    AppleGFXLinuxState *s = opaque;
    uint32_t value;

    /* libapplegfx-vulkan handles MMIO dispatch */
    value = lagfx_mmio_read(s->lagfx_dev, offset);

    trace_apple_gfx_read(offset, value);
    return value;
}

void
apple_gfx_mmio_write(void *opaque, hwaddr offset, uint64_t val, unsigned size)
{
    AppleGFXLinuxState *s = opaque;

    /* libapplegfx-vulkan handles MMIO dispatch */
    lagfx_mmio_write(s->lagfx_dev, offset, (uint32_t)val);

    trace_apple_gfx_write(offset, val);
}

static const MemoryRegionOps apple_gfx_mmio_ops = {
    .read = apple_gfx_mmio_read,
    .write = apple_gfx_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* ------ Device Initialization ------ */

void
apple_gfx_common_init(Object *obj, AppleGFXLinuxState *s)
{
    /* Default MMIO region size; libapplegfx may override */
    size_t mmio_range_size = 0x4000;  /* 16 KB default */

    trace_apple_gfx_common_init("apple-gfx-linux", mmio_range_size);

    memory_region_init_io(&s->iomem_gfx, obj, &apple_gfx_mmio_ops, s,
                          "apple-gfx-mmio", mmio_range_size);

    qemu_mutex_init(&s->task_mutex);
    QTAILQ_INIT(&s->tasks);

    s->cursor_show = true;
}

bool
apple_gfx_common_realize(AppleGFXLinuxState *s, DeviceState *dev,
                         lagfx_device_descriptor_t *desc, Error **errp)
{
    const AppleGFXDisplayMode *display_modes = apple_gfx_default_modes;
    uint32_t num_display_modes = ARRAY_SIZE(apple_gfx_default_modes);
    lagfx_display_descriptor_t disp_desc;
    char *errp_lagfx = NULL;

    if (apple_gfx_mig_blocker == NULL) {
        error_setg(&apple_gfx_mig_blocker,
                   "Migration state blocked by apple-gfx display device");
        if (migrate_add_blocker(&apple_gfx_mig_blocker, errp) < 0) {
            return false;
        }
    }

    /* Create the paravirt GPU device via libapplegfx */
    s->lagfx_dev = lagfx_device_new(desc, &errp_lagfx);
    if (!s->lagfx_dev) {
        error_setg(errp, "Failed to create lagfx_device: %s",
                   errp_lagfx ? errp_lagfx : "unknown");
        g_free(errp_lagfx);
        return false;
    }

    /* Prepare display descriptor */
    memset(&disp_desc, 0, sizeof(disp_desc));
    disp_desc.name = "QEMU display";
    disp_desc.size_mm_width = 400;   /* 20" display */
    disp_desc.size_mm_height = 300;

    disp_desc.modes = (const lagfx_display_mode_t *)
        (s->display_modes ? s->display_modes : display_modes);
    disp_desc.mode_count = s->display_modes ? s->num_display_modes : num_display_modes;

    /* Set up display callbacks */
    disp_desc.callbacks.opaque = s;
    disp_desc.callbacks.frame_ready = apple_gfx_frame_ready;
    disp_desc.callbacks.mode_changed = apple_gfx_mode_changed;
    disp_desc.callbacks.cursor_glyph = apple_gfx_cursor_glyph;
    disp_desc.callbacks.cursor_moved = apple_gfx_cursor_moved;
    disp_desc.callbacks.cursor_show = apple_gfx_cursor_show;

    /* Create display */
    s->lagfx_disp = lagfx_display_new(s->lagfx_dev, &disp_desc,
                                       0, /* port */
                                       next_pgdisplay_serial_num++,
                                       &errp_lagfx);
    if (!s->lagfx_disp) {
        error_setg(errp, "Failed to create lagfx_display: %s",
                   errp_lagfx ? errp_lagfx : "unknown");
        lagfx_device_free(s->lagfx_dev);
        g_free(errp_lagfx);
        return false;
    }

    /* Create QEMU console for display */
    static const GraphicHwOps apple_gfx_hw_ops = {
        .gfx_update = NULL,  /* TODO(Phase-1.B): implement */
        .gfx_update_async = true,
    };

    s->con = graphic_console_init(dev, 0, &apple_gfx_hw_ops, s);

    return true;
}

/* ------ Display Mode Property Getters/Setters ------ */

static void
apple_gfx_get_display_mode(Object *obj, Visitor *v,
                            const char *name, void *opaque, Error **errp)
{
    Property *prop = opaque;
    AppleGFXDisplayMode *mode = object_field_prop_ptr(obj, prop);
    char buffer[64];

    snprintf(buffer, sizeof(buffer),
             "%"PRIu16"x%"PRIu16"@%"PRIu16,
             mode->width_px, mode->height_px, mode->refresh_rate_hz);

    visit_type_str(v, name, &(char *){ buffer }, errp);
}

static void
apple_gfx_set_display_mode(Object *obj, Visitor *v,
                            const char *name, void *opaque, Error **errp)
{
    Property *prop = opaque;
    AppleGFXDisplayMode *mode = object_field_prop_ptr(obj, prop);
    const char *endptr;
    g_autofree char *str = NULL;
    int ret, val;

    if (!visit_type_str(v, name, &str, errp)) {
        return;
    }

    endptr = str;

    /* Parse width */
    ret = qemu_strtoi(endptr, &endptr, 10, &val);
    if (ret || val > UINT16_MAX || val <= 0) {
        error_setg(errp, "width in '%s' must be 1..65535", name);
        return;
    }
    mode->width_px = val;

    if (*endptr != 'x') {
        error_setg(errp, "format must be <width>x<height>@<rate>");
        return;
    }

    /* Parse height */
    ret = qemu_strtoi(endptr + 1, &endptr, 10, &val);
    if (ret || val > UINT16_MAX || val <= 0) {
        error_setg(errp, "height in '%s' must be 1..65535", name);
        return;
    }
    mode->height_px = val;

    if (*endptr != '@') {
        error_setg(errp, "format must be <width>x<height>@<rate>");
        return;
    }

    /* Parse refresh rate */
    ret = qemu_strtoi(endptr + 1, &endptr, 10, &val);
    if (ret || val > UINT16_MAX || val <= 0) {
        error_setg(errp, "refresh rate in '%s' must be positive", name);
        return;
    }
    mode->refresh_rate_hz = val;
}

const PropertyInfo qdev_prop_apple_gfx_display_mode = {
    .type = "display_mode",
    .description = "Display mode as <width>x<height>@<refresh-rate>. "
                   "Example: 3840x2160@60",
    .get = apple_gfx_get_display_mode,
    .set = apple_gfx_set_display_mode,
};
