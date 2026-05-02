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
 *
 * Last updated: 2026-05-01
 *
 * Cursor callbacks wired ✅ (commit 0511f24 + 69c063e)
 * Status: Docker host DOWN, can't verify noVNC visibility
 *
 * Cursor callback flow (2026-04-30):
 *   Guest opcode 0x13 (CmdDisplayCursorShow):
 *     → libapplegfx (ops_display_vchan.c) → apple_gfx_cursor_moved()
 *     → BH: apple_gfx_cursor_moved_bh() → dpy_mouse_set()
 *   Guest opcode 0x14 (CmdDisplayCursorGlyph):
 *     → libapplegfx (ops_display_vchan.c) → apple_gfx_cursor_glyph()
 *     → BGRA→RGBA conversion → dpy_cursor_define()
 *
 * Both opcodes travel on display vchan sub-channels (created via
 * opcode 0x04 CmdDefineChildFIFO), NOT the main display channel.
 * See paravirt-re/cursor-rendering-stage-20.md for pipeline.
 *
 * Current blocker: ABBA deadlock (WindowServer ↔ DisplayPipe)
 *   prevents full verification of cursor visibility in noVNC.
 *   - Thread 1 (WS): holds FB workloop gate, waits for
 *     accel[+0x88] IOLock in doSetDisplayMode.
 *   - Thread 2 (DisplayPipe): holds accel[+0x88] IOLock,
 *     waits for FB workloop gate in process_online.
 *   Fix attempts: 39a351c (defer online), aa91185 (wait 5 submits),
 *     40c1c7c (threshold=1), 7663ff1 (fix enabled flag) — NOT resolved.
 *   See journey/deadlock-abba-analysis.md.
 *
 * CURSOR VISIBILITY STATUS (2026-04-30): BLOCKED BY DEADLOCK
 *   - Cursor callbacks NOW FIRE (commit 0511f24 wires up dispatch)
 *   - But WindowServer hangs in ABBA deadlock before completing
 *     display initialization → cursor never becomes visible in noVNC
 *   - IOAccelDevice2=0, IOAccelShared2=0 (never created due to deadlock)
 *   - metal-test processes stuck in ?E/UN state (unkillable)
 *   - See paravirt-re/library/stage-20-cursor-status.md for details
 *
 * RE references:
 *   - paravirt-re/cursor-rendering-stage-20.md
 *   - paravirt-re/AppleParavirtDisplayPipe-vtable-decoded.md
 *   - paravirt-re/library/stage-20-cursor-status.md
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
#include "hw/qdev-properties.h"
#include "hw/pci/msi.h"
#include "hw/pci/pci.h"
#include "apple-gfx-linux.h"
#include "trace.h"

#define PG_PCI_BAR_MMIO 0
#define PG_PCI_MAX_MSI_VECTORS 32

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
bool
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

bool
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

/*
 * Frame-ready bottom half: pull the most recent rendered frame out of
 * libapplegfx-vulkan and push it into QEMU's DisplaySurface.
 *
 * Runs under the BQL because DisplaySurface mutation / dpy_gfx_update
 * is not safe to call from arbitrary BH contexts on its own. The
 * pending_frames counter is incremented by apple_gfx_frame_ready()
 * (which libapplegfx calls from its render path) and drained here one
 * at a time; if additional frames arrived while we were draining, we
 * re-schedule ourselves so the display stays current without stalling
 * the renderer.
 *
 * LAGFX_ERR_NO_FRAME is the expected return while the library's
 * Vulkan render path is still in progress (Phase 2.A/2.B territory):
 * treat it as a silent no-op and drop the frame token — the next
 * frame_ready callback will re-arm us. Any other non-OK status is
 * logged at LOG_GUEST_ERROR since it points at a real library or
 * shell bug.
 *
 * Integration-tested end-to-end via docker-macos/tests/verify-m4.sh
 * (M4: first-pixel gate). There is no reasonable unit test for this
 * path because it requires a running QEMU with a live QemuConsole +
 * an initialised lagfx_display_t; both are harness-scale fixtures.
 */
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
        /*
         * The library-reported stride should match DisplaySurface's
         * stride; warn once-per-mismatch if they diverge (plan
         * P2-R4). vkCmdCopyImageToBuffer -> linear BGRA8 buffer
         * yields width*4 pitch, which matches QEMU's default
         * DisplaySurface pitch on x86 hosts.
         */
        if (stride_out != 0 && stride_out != stride) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "apple-gfx: stride mismatch: surface=%zu "
                          "library=%zu (frame may tear)\n",
                          stride, stride_out);
        }
        qemu_log_mask(LOG_TRACE,
                      "apple-gfx: frame dispatched to DisplaySurface "
                      "(%zux%zu, stride=%zu)\n",
                      width_px, height_px, stride);
        dpy_gfx_update(s->con, 0, 0, (int)width_px, (int)height_px);
    } else if (r == LAGFX_ERR_NO_FRAME) {
        /*
         * Library has no new frame yet; the Vulkan submit/readback
         * pipeline will post one later via its own frame_ready
         * callback. No-op — do not log, this is the hot path during
         * early Phase 2 bring-up.
         */
    } else if (r != LAGFX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "apple-gfx: lagfx_display_read_frame failed "
                      "(status=%d)\n", (int)r);
    }

    /*
     * More frames queued up while we were servicing this one — chain
     * another BH so we don't leave the display stale. Guarded so we
     * don't busy-loop if the library is stuck returning NO_FRAME.
     */
    if (qatomic_read(&s->pending_frames) > 0) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(),
                                apple_gfx_frame_ready_bh, opaque);
    }
}

static void
apple_gfx_vblank_tick(void *opaque)
{
    AppleGFXLinuxState *s = opaque;
    lagfx_display_tick_vblank(s->lagfx_dev, s,
                              apple_gfx_write_memory);
    timer_mod(&s->vblank_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);
}

static void
apple_gfx_frame_ready(void *opaque)
{
    AppleGFXLinuxState *s = opaque;
    int prev;

    trace_apple_gfx_new_frame();

    /*
     * Drop frames if we're falling behind: cap pending_frames at 2 so
     * a runaway renderer can't blow the BH queue. One BH is enough to
     * drain; it will re-arm itself while work remains.
     *
     * This callback may run from a non-BQL library thread concurrently
     * with the drain BH, so all access to pending_frames is via
     * qatomic_* to match the BH's atomic read/sub.
     */
    if (qatomic_read(&s->pending_frames) >= 2) {
        return;
    }
    prev = qatomic_fetch_add(&s->pending_frames, 1);
    if (prev >= 1) {
        /* Previous BH still in flight; it will chain to this frame. */
        return;
    }

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
    /* Cursor glyph BH: QEMU requires BQL for dpy_cursor_define, but
     * apple_gfx_cursor_glyph() already calls it directly (BQL not
     * strictly required for cursor_define). This BH remains as a
     * placeholder for any follow-up actions (e.g., cursor position
     * sync) that may need the BQL in the future. */
    g_free(opaque);
}

/*
 * Cursor glyph callback — invoked by libapplegfx-vulkan when it
 * receives opcode 0x14 (CmdDisplayCursorGlyph) from the guest.
 *
 * RE context (paravirt-re/cursor-rendering-stage-20.md):
 *   0x14 is emitted by AppleParavirtDisplayPipe::updateCursorGlyph()
 *   with a 64×64 (or similar) ARGB8888 bitmap at glyphVA.
 *   The guest sends this on a display vchan sub-channel (created
 *   via opcode 0x04 CmdDefineChildFIFO), NOT the main display channel.
 *   See also paravirt-re/AppleParavirtDisplayPipe-vtable-decoded.md
 *   for the vtable dispatch to updateCursorGlyph.
 *
 * The guest's hardware-cursor path expects an ack after this opcode;
 * libapplegfx signals the stamp internally. If we don't call
 * dpy_cursor_define(), the guest falls back to software-cursor
 * rendering into the main framebuffer (lag, smear, or disappearance
 * under Metal composition).
 * See paravirt-re/library/stage-20-cursor-status.md for status.
 *
 * Cursor pipeline flow (2026-04-30):
 *   Guest: emits 0x14 CmdDisplayCursorGlyph
 *     → libapplegfx handler (ops_display_vchan.c)
 *     → apple_gfx_cursor_glyph() [this callback]
 *     → QEMU dpy_cursor_define() sets cursor image
 *     → noVNC picks up cursor data via VNC rich cursor encoding
 *
 * DEADLOCK BLOCKER: Even though cursor callbacks NOW FIRE
 * (commit 0511f24), the ABBA deadlock prevents WindowServer
 * from fully initializing display — cursor may not appear.
 *
 * Pixel format note: bgra_pixels is BGRA (kext emits ARGB but the
 * display pipe byte-swaps to BGRA for the host). QEMU cursor expects
 * RGBA, so we reorder in the loop below.
 */
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

/*
 * Cursor position BH — reads the display's cursor_position field
 * (updated by the 0x13 handler in libapplegfx) and pushes it to
 * QEMU's UI via dpy_mouse_set.
 *
 * The position is signed 16-bit in the 0x13 payload (RE-confirmed
 * at paravirt-re/re-followup-spec-gaps.md:1996) — this allows the
 * cursor to track off-screen during multi-display hand-off.
 * lagfx_display_cursor_position() returns the (x,y) stored by the
 * most recent 0x13 CmdDisplayCursorShow.
 */
static void
apple_gfx_cursor_moved_bh(void *opaque)
{
    AppleGFXLinuxState *s = opaque;
    lagfx_coord_t pos = lagfx_display_cursor_position(s->lagfx_disp);

    dpy_mouse_set(s->con, pos.x, pos.y, s->cursor_show);
}

/*
 * Cursor movement callback — invoked by libapplegfx-vulkan when
 * it receives opcode 0x13 (CmdDisplayCursorShow) from the guest.
 *
 * RE context (paravirt-re/cursor-rendering-stage-20.md):
 *   Emitted by AppleParavirtDisplayPipe::updateCursorState(u16 x,
 *   u16 y, bool visible). This is the hardware-cursor position +
 *   visibility update — NOT rendered through the main framebuffer
 *   pipeline, but as a separate overlay/compositing path in the
 *   display controller.
 *   See paravirt-re/AppleParavirtDisplayPipe-vtable-decoded.md
 *   for the vtable entry and paravirt-re/re-followup-spec-gaps.md
 *   for signed 16-bit position encoding at §1996.
 *
 * Cursor flow (2026-04-30):
 *   Guest: emits 0x13 CmdDisplayCursorShow
 *     → libapplegfx handler (ops_display_vchan.c)
 *     → apple_gfx_cursor_moved() [this callback] — triggers BH
 *     → apple_gfx_cursor_moved_bh() — reads cursor position
 *     → dpy_mouse_set(s->con, pos.x, pos.y, s->cursor_show)
 *     → QEMU VNC server picks up position + visibility
 *
 * DEADLOCK BLOCKER: WindowServer hangs in ABBA deadlock,
 *   so cursor position updates may not reach VNC in practice.
 *   See journey/deadlock-abba-analysis.md.
 *
 * Deferred to BH for BQL safety (QEMU console ops prefer BQL).
 */
static void
apple_gfx_cursor_moved(void *opaque)
{
    trace_apple_gfx_cursor_move();
    aio_bh_schedule_oneshot(qemu_get_aio_context(),
                             apple_gfx_cursor_moved_bh, opaque);
}

/*
 * Cursor visibility callback — invoked by libapplegfx-vulkan when
 * it receives opcode 0x13 (CmdDisplayCursorShow) with the 'visible'
 * field.
 *
 * RE context (paravirt-re/cursor-rendering-stage-20.md,
 * paravirt-re/AppleParavirtDisplayPipe-vtable-decoded.md):
 *   The guest's updateCursorState() emits one 0x13 per movement,
 *   with visible=1 during normal use. If we fail to honor visibility,
 *   the cursor may appear when it shouldn't (e.g., during display
 *   reconfiguration) or disappear unexpectedly.
 *   See paravirt-re/library/stage-20-cursor-status.md for stage 20%
 *   status: cursor callbacks wired but deadlock (§BLOCKER) prevents
 *   full verification.
 *
 * Cursor visibility flow (2026-04-30):
 *   Guest: emits 0x13 CmdDisplayCursorShow
 *     → libapplegfx handler (ops_display_vchan.c)
 *     → apple_gfx_cursor_show() [this callback]
 *     → sets s->cursor_show = show
 *     → BH: apple_gfx_cursor_moved_bh()
 *     → dpy_mouse_set(s->con, pos.x, pos.y, s->cursor_show)
 *     → QEMU VNC sends cursor visibility to VNC client
 *
 * DEADLOCK BLOCKER: WindowServer hangs in ABBA deadlock,
 *   so cursor may not be visible in practice (WS never completes
 *   display initialization to show cursor).
 *   See journey/deadlock-abba-analysis.md.
 */
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

    /*
     * Set up display callbacks — these are invoked by libapplegfx-vulkan
     * when the guest emits display-pipe opcodes on the vchan.
     *
     * Callback → Guest opcode mapping (RE-confirmed):
     *   cursor_glyph  ← 0x14 CmdDisplayCursorGlyph
     *     (AppleParavirtDisplayPipe::updateCursorGlyph @ kext+0x145619c4)
     *   cursor_moved  ← 0x13 CmdDisplayCursorShow (position + visibility)
     *     (AppleParavirtDisplayPipe::updateCursorState @ kext+0x14562094)
     *   cursor_show   ← 0x13 CmdDisplayCursorShow (visibility field)
     *
     * Both 0x13/0x14 are sent on display vchan sub-channels
     * (child FIFOs created via opcode 0x04 CmdDefineChildFIFO), not
     * the main display channel.
     *
     * Without these callbacks wired, the guest's hardware-cursor path
     * falls back to software-cursor rendering into the main framebuffer
     * (paravirt-re/re-followup-spec-gaps.md:2012-2017).
     */
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

    /* VBlank timer: ticks at ~60 Hz to advance the guest's vblank
     * counter via lagfx_ops_display_tick_vblank(). WindowServer
     * waits on this counter before submitting display updates. */
    timer_init_ns(&s->vblank_timer, QEMU_CLOCK_VIRTUAL,
                  apple_gfx_vblank_tick, s);
    timer_mod(&s->vblank_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);

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
