/*
 * QEMU Apple ParavirtualizedGraphics.framework device — transport-agnostic core (Linux C port).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright © 2023-2026 Amazon/QEMU contributors
 */

#include "qemu/osdep.h"
#include "qemu/lockable.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qapi/visitor.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/aio-wait.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "migration/blocker.h"
#include "ui/console.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci.h"
#include "apple-gfx-linux.h"
#include "trace.h"

#define PG_PCI_BAR_MMIO 0
#define PG_PCI_MAX_MSI_VECTORS 32

/* AppleGFXDisplayMode: 3× uint16_t (6 bytes). lagfx_display_mode_t: 3× uint32_t
 * (12 bytes). Direct cast reads garbage + walks past array end. Convert explicitly. */
static void apple_gfx_modes_to_lagfx(lagfx_display_mode_t *out,
                                     const AppleGFXDisplayMode *in,
                                     unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        out[i].width_px  = in[i].width_px;
        out[i].height_px = in[i].height_px;
        out[i].refresh_rate_hz = in[i].refresh_rate_hz;
    }
}

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
lagfx_task_t *
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
                       "[lagfx] apple-gfx: lagfx_task_create(vm_size=0x%" PRIx64
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

void
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
                       "[lagfx] apple-gfx: destroy_task for unknown handle %p\n", task);
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
 * reflected in the task VA range" — verified for read-mostly ranges only.
 */
bool
apple_gfx_map_memory(void *opaque, lagfx_task_t *task,
                     uint64_t virtual_offset,
                     const lagfx_physical_range_t *ranges,
                     size_t range_count, bool read_only)
{
    AppleGFXLinuxState *s = opaque;
    AppleGFXLinuxTask *linux_task = NULL;
    AppleGFXLinuxTask *it;
    /*
     * Track per-range state for proper unwind on partial failure:
     *  - mapped[i]:    lagfx_task_map_host_memory succeeded for this range
     *                  (so it must be unmapped on rollback).
     *  - new_region[i]: the MemoryRegion was first ref'd by *this* call
     *                   (so its ref must be dropped on rollback). A region
     *                   already in linux_task->mapped_regions before we
     *                   started keeps its existing ref.
     */
    g_autofree bool *mapped = g_new0(bool, range_count > 0 ? range_count : 1);
    g_autofree MemoryRegion **new_region =
        g_new0(MemoryRegion *, range_count > 0 ? range_count : 1);
    uint64_t base_virtual_offset = virtual_offset;
    uint64_t cur_virtual_offset = virtual_offset;
    bool success = true;
    size_t fail_index = 0;

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
                       "[lagfx] apple-gfx: map_memory for unknown task handle %p\n",
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
                          "[lagfx] apple-gfx: map_memory: GPA 0x%" PRIx64
                          " len 0x%" PRIx64 " not directly accessible\n",
                          range->guest_physical_address, range->length);
            success = false;
            fail_index = i;
            break;
        }

        /* Take a fresh ref only the first time we see this region in
         * this task; track it so we can drop it on rollback. */
        if (!g_ptr_array_find(linux_task->mapped_regions, region, NULL)) {
            g_ptr_array_add(linux_task->mapped_regions, region);
            memory_region_ref(region);
            new_region[i] = region;
        }

       if (!lagfx_task_map_host_memory(task, cur_virtual_offset, host_ptr,
                                          range->length, read_only)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "[lagfx] apple-gfx: lagfx_task_map_host_memory failed"
                          " at task_offset=0x%" PRIx64 " len=0x%" PRIx64 "\n",
                          cur_virtual_offset, range->length);
            success = false;
            fail_index = i;
            break;
        }
        mapped[i] = true;
        cur_virtual_offset += range->length;
    }

    if (!success) {
        /* Roll back everything done in this call so the caller sees an
         * all-or-nothing map. */
        uint64_t rollback_offset = base_virtual_offset;
        for (size_t i = 0; i < fail_index; i++) {
            if (mapped[i]) {
                lagfx_task_unmap(task, rollback_offset, ranges[i].length);
            }
            rollback_offset += ranges[i].length;
        }
        for (size_t i = 0; i <= fail_index && i < range_count; i++) {
            if (new_region[i]) {
                g_ptr_array_remove_fast(linux_task->mapped_regions,
                                        new_region[i]);
                memory_region_unref(new_region[i]);
            }
        }
    }

    return success;
}

bool
apple_gfx_unmap_memory(void *opaque, lagfx_task_t *task,
                       uint64_t virtual_offset, uint64_t length)
{
    (void)opaque;

    trace_apple_gfx_unmap_memory(task, virtual_offset, length);

  if (!lagfx_task_unmap(task, virtual_offset, length)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                       "[lagfx] apple-gfx: lagfx_task_unmap failed"
                       " at offset=0x%" PRIx64 " len=0x%" PRIx64 "\n",
                      virtual_offset, length);
        return false;
    }
    return true;
}

bool
apple_gfx_read_memory(void *opaque, uint64_t guest_physical_address,
                      uint64_t length, void *dst)
{
    MemTxResult r;

    /*
     * opaque holds the AppleGFXLinuxState but the DMA engine is
     * addressed directly via address_space_memory, so the caller
     * state is not needed here. Keep the argument for ABI
     * symmetry with the other libapplegfx-vulkan callbacks.
     */
    (void)opaque;

    trace_apple_gfx_read_memory(guest_physical_address, length, dst);

    /* Use QEMU's DMA engine to read guest RAM into dst */
    r = dma_memory_read(&address_space_memory, guest_physical_address,
                        dst, length, MEMTXATTRS_UNSPECIFIED);

    return (r == MEMTX_OK);
}

bool
apple_gfx_write_memory(void *opaque, uint64_t guest_physical_address,
                       uint64_t length, const void *src)
{
    MemTxResult r;

    (void)opaque;

    trace_apple_gfx_write_memory(guest_physical_address, length, src);

    r = dma_memory_write(&address_space_memory, guest_physical_address,
                         src, length, MEMTXATTRS_UNSPECIFIED);

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

void
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

/* BQL is required for dpy_gfx_update + DisplaySurface mutation. */
static void
apple_gfx_frame_ready_bh(void *opaque)
{
    AppleGFXLinuxState *s = opaque;
    void *dst;
    size_t stride;
    size_t width_px;
    size_t height_px;
    size_t dst_size;
    size_t stride_out = 0;
    bool new_frame = false;
    lagfx_status_t r;

    BQL_LOCK_GUARD();

    if (qatomic_read(&s->pending_frames) <= 0 || !s->lagfx_disp) {
        if (qatomic_read(&s->pending_frames) > 0) {
            qatomic_fetch_sub(&s->pending_frames, 1);
        }
        return;
    }
    qatomic_fetch_sub(&s->pending_frames, 1);

    if (!s->surface) {
        s->surface = qemu_create_displaysurface(1920, 1080);
        dpy_gfx_replace_surface(s->con, s->surface);
    }

    dst      = surface_data(s->surface);
    stride   = surface_stride(s->surface);
    width_px = surface_width(s->surface);
    height_px = surface_height(s->surface);
    dst_size = stride * height_px;

    r = lagfx_display_read_frame(s->lagfx_disp, dst, dst_size,
                                 &stride_out, &new_frame);

    if (r == LAGFX_OK && new_frame) {
        if (stride_out != 0 && stride_out != stride) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "[lagfx] apple-gfx: stride mismatch: surface=%zu "
                          "library=%zu (frame may tear)\n",
                          stride, stride_out);
        }
        qemu_log_mask(LOG_TRACE,
                      "[lagfx] apple-gfx: frame dispatched to DisplaySurface "
                      "(%zux%zu, stride=%zu)\n",
                      width_px, height_px, stride);
        dpy_gfx_update(s->con, 0, 0, (int)width_px, (int)height_px);
    } else if (r == LAGFX_ERR_NO_FRAME) {
        /* Hot path while the renderer is still warming up — silent. */
    } else if (r != LAGFX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "[lagfx] apple-gfx: lagfx_display_read_frame failed "
                      "(status=%d)\n", (int)r);
    }

    if (qatomic_read(&s->pending_frames) > 0) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(),
                                apple_gfx_frame_ready_bh, opaque);
    }
}

static void
apple_gfx_vblank_tick(void *opaque)
{
    AppleGFXLinuxState *s = opaque;

    trace_apple_gfx_vblank_tick();

    /* Tick may have been queued before unrealize completed; lagfx_dev
     * is NULLed on teardown — skip the call to avoid a UAF. */
    if (!s->lagfx_dev) {
        return;
    }

    if (!s->initial_surface_pushed) {
        dpy_gfx_update_full(s->con);
        s->initial_surface_pushed = true;
    }

  lagfx_display_tick_vblank(s->lagfx_dev, s,
                               apple_gfx_write_memory, apple_gfx_read_memory);
    timer_mod(&s->vblank_timer,
              qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
              NANOSECONDS_PER_SECOND / 60);
}

static void
apple_gfx_frame_ready(void *opaque)
{
    AppleGFXLinuxState *s = opaque;
    uint32_t prev;

    /* Cap pending at 2 with two-step cmpxchg.
     *  - cmpxchg(0,1): no work in flight, schedule one BH.
     *  - cmpxchg(1,2): one in flight, queue exactly one more.
     *  - prev >= 2:    already capped, drop this notification.
     * Both reads come from cmpxchg's atomic return value — never a
     * non-atomic re-read of pending_frames. */
    prev = qatomic_cmpxchg(&s->pending_frames, 0, 1);
    if (prev != 0) {
        prev = qatomic_cmpxchg(&s->pending_frames, 1, 2);
        if (prev != 1) {
            /* prev was 0 (a concurrent caller already drained), or
             * prev was >=2 (already at cap). Either way: nothing to do. */
            return;
        }
        /* We bumped 1 -> 2; the in-flight BH will reschedule itself
         * via the tail of apple_gfx_frame_ready_bh, so don't schedule
         * another one here. */
        return;
    }

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

/*
 * Cursor glyph callback (libapplegfx opcode 0x14
 * CmdDisplayCursorGlyph). Invoked from a libapplegfx render thread
 * with no BQL held; s->cursor and dpy_cursor_define() require the
 * BQL, so we build the QEMUCursor here, transfer ownership to a BH,
 * and let the BH do the swap + push.
 *
 * Pixel format: bgra_pixels arrives as BGRA (the display pipe
 * byte-swaps the kext's ARGB before handing it off); QEMUCursor
 * expects RGBA, so we reorder while copying.
 */
typedef struct {
    AppleGFXLinuxState *s;
    QEMUCursor *cursor;
} AppleGFXCursorGlyphJob;

static void
apple_gfx_cursor_glyph_bh(void *opaque)
{
    AppleGFXCursorGlyphJob *job = opaque;
    AppleGFXLinuxState *s = job->s;

    BQL_LOCK_GUARD();

    if (s->cursor) {
        cursor_unref(s->cursor);
    }
    s->cursor = job->cursor;
    dpy_cursor_define(s->con, s->cursor);

    g_free(job);
}

static void
apple_gfx_cursor_glyph(void *opaque,
                         const uint8_t *bgra_pixels,
                         uint32_t width, uint32_t height,
                         lagfx_coord_t hotspot)
{
    AppleGFXLinuxState *s = opaque;
    AppleGFXCursorGlyphJob *job;
    QEMUCursor *cursor;
    uint32_t *dest_px;
    const uint8_t *src_px;

    /* BPP is always 32 for QEMU cursors. */
    trace_apple_gfx_cursor_set(32, width, height);

    cursor = cursor_alloc(width, height);
    cursor->hot_x = hotspot.x;
    cursor->hot_y = hotspot.y;

    dest_px = cursor->data;
    src_px = bgra_pixels;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            *dest_px = (src_px[2] << 16u) |  /* R */
                       (src_px[1] <<  8u) |  /* G */
                       (src_px[0] <<  0u) |  /* B */
                       (src_px[3] << 24u);   /* A */
            ++dest_px;
            src_px += 4;
        }
    }

    job = g_new0(AppleGFXCursorGlyphJob, 1);
    job->s = s;
    job->cursor = cursor;

    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                             apple_gfx_cursor_glyph_bh, job);
}

static void
apple_gfx_cursor_moved_bh(void *opaque)
{
    AppleGFXLinuxState *s = opaque;
    lagfx_coord_t pos = lagfx_display_cursor_position(s->lagfx_disp);

    BQL_LOCK_GUARD();
    dpy_mouse_set(s->con, pos.x, pos.y, s->cursor_show);
}

/* libapplegfx opcode 0x13 CmdDisplayCursorShow (position field). */
static void
apple_gfx_cursor_moved(void *opaque)
{
    trace_apple_gfx_cursor_move();
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                             apple_gfx_cursor_moved_bh, opaque);
}

/* libapplegfx opcode 0x13 CmdDisplayCursorShow (visible field). */
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

    trace_apple_gfx_common_init(object_get_typename(obj), mmio_range_size);

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

    /* Convert AppleGFXDisplayMode (3× uint16_t) to lagfx_display_mode_t
     * (3× uint32_t). Can't cast directly — different layout. */
    static lagfx_display_mode_t converted_modes[4];
    unsigned mode_count = s->display_modes ? s->num_display_modes : num_display_modes;
    const AppleGFXDisplayMode *src_modes = s->display_modes ? s->display_modes : display_modes;
    apple_gfx_modes_to_lagfx(converted_modes, src_modes, mode_count);
    disp_desc.modes = converted_modes;
    disp_desc.mode_count = mode_count;

    disp_desc.callbacks.opaque = s;
    disp_desc.callbacks.frame_ready = apple_gfx_frame_ready;
    disp_desc.callbacks.mode_changed = apple_gfx_mode_changed;
    disp_desc.callbacks.cursor_glyph = apple_gfx_cursor_glyph;
    disp_desc.callbacks.cursor_moved = apple_gfx_cursor_moved;
    disp_desc.callbacks.cursor_show = apple_gfx_cursor_show;

    static const GraphicHwOps apple_gfx_hw_ops = {
        .gfx_update = NULL,
        .gfx_update_async = true,
    };

    s->con = graphic_console_init(dev, 0, &apple_gfx_hw_ops, s);

    s->surface = qemu_create_displaysurface(1920, 1080);
    dpy_gfx_replace_surface(s->con, s->surface);

    /* Create display — may synchronously call apple_gfx_mode_changed.
     * Console + surface above must already be set. */
    s->lagfx_disp = lagfx_display_new(s->lagfx_dev, &disp_desc,
                                      0, /* port */
                                      next_pgdisplay_serial_num++,
                                      &errp_lagfx);
    if (!s->lagfx_disp) {
        error_setg(errp, "Failed to create lagfx_display: %s",
                   errp_lagfx ? errp_lagfx : "unknown");

        /* dpy_gfx_replace_surface(NULL) takes ownership and frees the
         * prior surface; do NOT call qemu_free_displaysurface() after
         * it (double-free). */
        if (s->con && s->surface) {
            dpy_gfx_replace_surface(s->con, NULL);
            s->surface = NULL;
        }

        /* Unregister migration blocker so a retry can re-add it. */
        migrate_del_blocker(&apple_gfx_mig_blocker);

        /* Free lagfx_dev *before* NULLing the field — passing NULL to
         * lagfx_device_free would leak the underlying allocation. */
        lagfx_device_free(s->lagfx_dev);
        s->lagfx_dev = NULL;

        g_free(errp_lagfx);
        return false;
    }

    /* 60 Hz vblank tick. Also performs the initial dpy_gfx_update_full —
     * doing it from realize segfaults because VNC/SDL backends aren't
     * wired into the QemuConsole's listener chain until vCPU start. */
    timer_init_ns(&s->vblank_timer, QEMU_CLOCK_REALTIME,
                  apple_gfx_vblank_tick, s);
   uint64_t next_fire = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
                          NANOSECONDS_PER_SECOND / 60;
    timer_mod(&s->vblank_timer, next_fire);
    trace_apple_gfx_vblank_timer_registered(next_fire);

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
