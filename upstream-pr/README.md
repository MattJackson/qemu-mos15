# Upstream PR staging area

Four discrete patch packages destined for
[qemu-project/qemu](https://gitlab.com/qemu-project/qemu) upstream,
staged here before submission. Each subdirectory is a
self-contained PR: cover letter (`SERIES.md`), numbered
`git format-patch`-compatible files, GitHub-style PR
description (`PR_DESCRIPTION.md`), and a testing recipe
(`TESTING.md`).

| Package | Subject | Size | Status |
|---------|---------|-----:|--------|
| [A](./applesmc-fix/) | Fix `GET_KEY_BY_INDEX` and populate boot keys | 4 patches | **Ready to submit** |
| [B](./apple-gfx-pci-linux/) | Linux-host port of `apple-gfx-pci` | 8 patches | **Draft; blocked on libapplegfx-vulkan packaging** |
| [C](./vmware-svga-caps/) | VMware SVGA II capability bits + 5K cap | 4 patches | **Ready to submit** |
| [D](./usb-hid-apple-ids/) | USB HID Apple vendor IDs | 3 patches | **Ready to submit** |

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

### B - apple-gfx-pci Linux port (blocked)

Companion to Phil Dennis-Jordan's upstream apple-gfx.m /
apple-gfx-pci.m work. Adds a Linux C port that drives the
same PGDevice protocol via `libapplegfx-vulkan` (a Mesa
lavapipe-backed shell reimplementation). Zero upstream
files are modified; additive only.

Dependencies: **hard blocker on `libapplegfx-vulkan`
packaging**. See
[apple-gfx-pci-linux/LIBAPPLEGFX_DEPENDENCY.md](./apple-gfx-pci-linux/LIBAPPLEGFX_DEPENDENCY.md).

Secondary blockers: in-tree EDK2 build for the option ROM
(currently a development placeholder); review by the
`apple-gfx.m` author.

Ready to submit: **no** - at least one of the dependency
options must land first.

### C - vmware_vga capability bits (ready)

Advertises capability bits the modern VMware SVGA II drivers
expect (PITCHLOCK, EXTENDED_FIFO, 8BIT_EMULATION, ALPHA_BLEND,
ALPHA_CURSOR) and raises the software-side resolution cap
from 2368x1770 to 5120x2880. All of the underlying machinery
is already present in the upstream device; this series only
flips the capability bits.

Dependencies: none.
Ready to submit: **yes, immediately**.

### D - USB HID Apple vendor IDs (ready)

Adds three opt-in wrapper devices - `apple-kbd`,
`apple-mouse`, `apple-tablet` - that inherit from
`usb-kbd` / `usb-mouse` / `usb-tablet` respectively and
override only the USB enumeration-time descriptor (Apple
vendor 0x05ac, real Apple product IDs, Apple Inc.
manufacturer / product strings). The HID report descriptor
and data-handling paths are reused 1:1 from the parent
devices.

Zero behaviour change for existing users: `-device
usb-kbd` / `usb-mouse` / `usb-tablet` and their vmstate
are unchanged. Migration of existing VMs is unaffected.

Motivation: macOS guests run the Keyboard Setup Assistant
at first boot for any non-Apple HID keyboard, which costs
3-5 minutes on interactive installs and hangs headless
(VNC / SPICE) installs indefinitely. The new devices let
macOS-guest users opt into Apple-identity HID without
affecting any other guest.

Supersedes an earlier single-patch draft that re-ID'd the
base devices globally.

Dependencies: none.
Ready to submit: **yes, immediately**.

## Submission order recommendation

1. **Package A** (applesmc) first - clearest bug, measurable
   impact, cleanest diff.
2. **Package C** (vmware_vga) alongside or immediately after -
   similarly uncontroversial.
3. **Package D** (USB HID apple-kbd / apple-mouse /
   apple-tablet wrappers) alongside A and C - additive, zero
   behaviour change for existing users.
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
