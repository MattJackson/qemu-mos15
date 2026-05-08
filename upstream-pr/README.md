# Upstream PR staging area

Five discrete patch packages destined for
[qemu-project/qemu](https://gitlab.com/qemu-project/qemu) upstream,
staged here before submission. Each subdirectory is a
self-contained PR: cover letter (`SERIES.md`), numbered
`git format-patch`-compatible files, GitHub-style PR
description (`PR_DESCRIPTION.md`), and a testing recipe
(`TESTING.md`).

| Package | Subject | Size | Status |
|---------|---------|-----:|--------|
| [A](./applesmc-fix/) | Fix `GET_KEY_BY_INDEX` and populate Apple SMC key set | 2 patches | **SUBMITTED 2026-05-06**, **v2 sent 2026-05-07** addressing Maydell review ([lore v1](https://lore.kernel.org/qemu-devel/20260507040153.14565-1-matthew@pq.io/), [v2 search](https://lore.kernel.org/qemu-devel/20260507152020.48728-1-matthew@pq.io/)) |
| [B](./apple-gfx-pci-linux/) | Linux-host port of `apple-gfx-pci` | 9 patches | **Blocked-ready** (library at `8edc43c`, packaging-path decision pending) — internal mos final-product helper, hold |
| [C](./vmware-svga-caps/) | VMware SVGA II capability bits + 5K cap | 4 patches | **Ready to submit** |
| ~~D~~ | ~~USB HID Apple vendor IDs~~ | — | **WITHDRAWN 2026-05-07** — descriptor-only wrappers were a wrong-shape solution; broke macOS recovery's HID stack. Superseded by Package E. |
| [E](./apple-magic-hid/) | Apple Magic Keyboard + Magic Trackpad USB-mode emulators | 2 patches | **Ready to submit 2026-05-08** — real-protocol replacement for withdrawn Package D; binds `AppleUSBTopCaseHIDDriver` at probe score 90000; descriptors byte-identical to real hardware |

Ready-to-submit: A, C, E (3 of 5). B is "blocked-ready":
the library side (`libapplegfx-vulkan`) has reached a stable
public API at commit `8edc43c` (Phase 2.B complete —
clear-colour render target + readback, displays render
end-to-end at the library level today); the QEMU-side
frame-readback BH is wired live; submission is waiting only
on the library packaging path (system package / meson
subproject / vendored submodule — see Package B's
`LIBAPPLEGFX_DEPENDENCY.md`). Precedent for upstream fork
submissions from this tree: our OpenCorePkg submission
([acidanthera/OpenCorePkg PR #600](https://github.com/acidanthera/OpenCorePkg/pull/600), similar hand-off shape) — the
next step for B is a discussion with the upstream maintainer
on which of the three library-packaging options they prefer
to review.

## Per-package status

### A - applesmc fix (ready)

Fixes a real, reproducible macOS-guest bug:
`APPLESMC_GET_KEY_BY_INDEX_CMD` returns zeros, which macOS
interprets as `kSMCSpuriousData` and retries at ~1800
errors/sec, burning `kernel_task` at 70 % CPU and
`WindowServer` at 509 %. The four-patch series implements
real iteration, adds `#KEY`, populates the sensor table with
realistic iMac20,1 values, and fills in the boot-required
key set.

Dependencies: none.
Ready to submit: **yes, immediately**.

### B - apple-gfx-pci Linux port (blocked-ready)

Companion to Phil Dennis-Jordan's upstream apple-gfx.m /
apple-gfx-pci.m work. Adds a Linux C port that drives the
same PGDevice protocol via `libapplegfx-vulkan` (a Mesa
lavapipe-backed shell reimplementation). Zero upstream
files are modified; additive only.

Nine patches total, adding the device, its `gpu_cores=N`
tunable, the meson + Kconfig gate, a placeholder option
ROM blob in pc-bios, and the Linux-port trace-events block.

**Library-side state** (as of this revision):
`libapplegfx-vulkan` at commit `8edc43c` implements Phase
1.A through Phase 2.B:

  * Task-memory management (memfd + mmap(MAP_FIXED), with
    mremap-based page aliasing for coherence).
  * MMIO dispatch with P0 + P1 + display-plane opcodes.
  * Vulkan init + command pool + empty-submit round-trip.
  * Vulkan clear-colour render target + image-to-buffer
    readback, surfaced via the `frame_ready` callback.

The full library test suite (`protocol-dispatch`,
`vulkan-init`, `vulkan-command`, `vulkan-render`,
`memory-coherence`, etc.) runs ~277 `CHECK` assertions on
a Linux host with Mesa lavapipe installed; zero failures.
The protocol-dispatch suite alone covers 207 assertions on
the opcode decoder.

On the QEMU side the frame-readback BH is now live
(previously a draft): displays render clear-colour
end-to-end at the library level today. Runtime surface:
`-device apple-gfx-pci,gpu_cores=N` (with optional
`-object memory-backend-memfd,share=on` for fast-path
task-VA aliasing).

Dependencies: **blocked on `libapplegfx-vulkan`
packaging**. See
[apple-gfx-pci-linux/LIBAPPLEGFX_DEPENDENCY.md](./apple-gfx-pci-linux/LIBAPPLEGFX_DEPENDENCY.md).
The library is API-stable at the surface this series uses,
so the decision at this point is procedural (which of the
three packaging paths to review) rather than technical. The
recommended path for the initial PR is Option 3 (vendored
submodule under `subprojects/libapplegfx-vulkan/`).

Secondary blockers: in-tree EDK2 build for the option ROM
(currently a development placeholder); review by the
`apple-gfx.m` author.

Ready to submit: **library ready, packaging decision
pending.** Next action: open a discussion with the upstream
apple-gfx maintainer on which of the three library
bundling paths they want to review first.

### C - vmware_vga capability bits (ready)

Advertises capability bits the modern VMware SVGA II drivers
expect (PITCHLOCK, EXTENDED_FIFO, 8BIT_EMULATION, ALPHA_BLEND,
ALPHA_CURSOR) and raises the software-side resolution cap
from 2368x1770 to 5120x2880. All of the underlying machinery
is already present in the upstream device; this series only
flips the capability bits.

Dependencies: none.
Ready to submit: **yes, immediately**.

### ~~D~~ - USB HID Apple vendor IDs (WITHDRAWN 2026-05-07)

Withdrawn: descriptor-only wrappers were a wrong-shape
solution. Apple's `AppleUSBTopCaseHIDDriver` claims the
device by VID/PID/strings but then refuses to bind because
the report descriptor is standard HID boot-protocol rather
than Apple's vendor-defined `UsagePage 0xff00` protocol.
Result on iMac20,1 SMBIOS guests: dangling unclaimed
device, "Power on Bluetooth Keyboard" recovery UI, all
input lost. Replaced by Package E.

### E - Apple Magic Keyboard + Magic Trackpad emulators (ready 2026-05-08)

Two new self-contained USB-HID devices in `hw/usb/dev-hid.c`:

  * **apple-magic-keyboard** (PID 0x026c) — composite
    device, two HID interfaces. Interface 0 carries Apple's
    vendor HID protocol on `UsagePage 0xff00` (vendor input
    report IDs 0xe0 / 0x9a / 0x90 + 1 Hz battery
    heartbeat); interface 1 is a standard HID Boot Keyboard
    wired to QEMU's input subsystem via
    `qemu_input_handler_register`. Descriptors are
    byte-identical to a real Magic Keyboard with Numeric
    Keypad in USB-cable mode.
  * **apple-magic-tablet** (PID 0x0265) — single vendor HID
    interface emulating the Magic Trackpad 2 boot-mouse
    face. Two input reports: 1 Hz heartbeat + boot-mouse
    pointer frame (~66 Hz cadence). 30 ms idle timer
    flips the surface state from touching to lifted.

Apple HID driver chain
(`AppleUSBTopCaseHIDDriver` → `AppleDeviceManagementHIDEvent
Service` → `AppleUserHIDEventDriver`) binds at probe score
90000 on macOS 15.7.5 recovery. Setup Assistant does not
appear. Visible keystroke proof captured via QMP `send-key`
advancing recovery's language-picker UI.

Zero behaviour change for existing users: `-device usb-kbd`
/ `usb-mouse` / `usb-tablet` and their vmstate are
unchanged. Migration of existing VMs is unaffected.

Vendor multitouch protocol (per-finger absolute frames) is
gated behind a vendor-enable SET_REPORT macOS sends after
enumeration; out of scope for v1, follow-up series will add
it once the vendor-enable sequence is reverse-engineered.

Dependencies: none.
Ready to submit: **yes, pending operator-driven
`git send-email` from postfix on classe** (same workflow as
Patch A on 2026-05-06). CC list to be populated by running
`scripts/get_maintainer.pl` on the patches from a full QEMU
tree (this fork lacks the scripts/ directory; use the
operator's classe checkout). Default CCs: `qemu-devel@nongnu.org`,
`qemu-trivial@nongnu.org`, plus `hw/usb` maintainer (Gerd
Hoffmann <kraxel@redhat.com>).

## Submission order recommendation

1. **Package A** (applesmc) first - clearest bug, measurable
   impact, cleanest diff.
2. **Package C** (vmware_vga) alongside or immediately after -
   similarly uncontroversial.
3. **Package E** (apple-magic-keyboard / apple-magic-tablet)
   alongside A and C - additive, zero behaviour change for
   existing users; supersedes withdrawn Package D.
4. **Package B** (apple-gfx-pci-linux) last, once the
   dependency story is resolved.

## Commit style

All upstream commits follow qemu-project convention:

  * `<subsystem>: <imperative subject>` subject line
    (e.g. `hw/misc/applesmc: fix GET_KEY_BY_INDEX ...`).
  * Body wrapped at roughly 75 columns.
  * `Signed-off-by: Matthew Jackson <matthew@pq.io>`
    trailer on every commit.
  * `Reported-by:` / `Tested-by:` where applicable.
  * **No** `Co-Authored-By: Claude ...` trailers on
    upstream-destined commits.

## Local cleanup commit

HEAD of the source tree contains a cleanup commit
(`43dcccb cleanup: strip fork-local identifiers from
custom patches`) that replaces `mos15:` comment prefixes
and `mos15-smc:` stderr strings with neutral,
subsystem-scoped phrasing, and routes non-fatal debug
output through `qemu_log_mask` instead of `fprintf(stderr)`.
The upstream patches in this directory are generated
against the post-cleanup tree and the upstream QEMU master
reference, so there are no fork-local identifiers in any
of the patch files.

Fork-local identifiers found and cleaned across the
custom files:
  * `hw/misc/applesmc.c`: 24
  * `hw/display/vmware_vga.c`: 2
  * `hw/display/apple-gfx-linux.h`: 1

`hw/usb/dev-hid.c` previously carried 13 fork-local
descriptor-string edits ("mos15:" comments and Apple IDs
baked into the base devices' descriptors). Those have been
reverted to upstream verbatim; the Apple identity now lives
in the opt-in `apple-kbd` / `apple-mouse` / `apple-tablet`
wrapper types added at the bottom of the same file (see
Package D).
