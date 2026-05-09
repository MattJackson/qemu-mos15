# apple-magic-hid: add USB-mode emulators for Apple Magic Keyboard and Mighty Mouse

**Status: READY TO SEND** (regenerated 2026-05-10 against vanilla qemu
11.0.0). Patches verified to apply cleanly to upstream, reproduce
current downstream `dev-hid.c` exactly, and pass end-to-end visual
keystroke proof via the `./mos verify 3` Tier B input regression.

> **Note on naming.** An earlier draft of this series (2026-05-08)
> shipped `apple-magic-tablet` emulating Magic Trackpad 2 (PID 0x0265).
> The Magic Trackpad path requires vendor-multitouch wire bytes we
> couldn't verify without another USB capture, and guessed bytes
> caused AppleHIDStack hangs during recovery boots. The 2026-05-09
> fresh-eyes pivot retargeted at Apple Mighty Mouse (PID 0x0304) — a
> standard USB HID boot-mouse-with-scroll device with no vendor
> protocol — and `apple-magic-tablet` is now a backward-compat legacy
> TypeInfo alias that resolves to the canonical `apple-mighty-mouse`
> implementation. See "Why Mighty Mouse, not Magic Trackpad" in the
> cover letter.

## Bug / motivation

QEMU has no USB-HID device that macOS recognises as a real Apple HID
peripheral. The generic `usb-kbd` / `usb-tablet` work for typical
guests, but on macOS they hit two practical issues:

1. macOS runs Keyboard Setup Assistant on first boot for any
   non-Apple HID keyboard, which costs 3-5 minutes on interactive
   installs and effectively hangs headless (VNC / SPICE) installs
   that have no way to dismiss the wizard.

2. During recovery / install, the Apple HID stack
   (`AppleUSBTopCaseHIDDriver`, `AppleDeviceManagementHIDEvent
   Service`, `AppleHIDKeyboardEventDriverV2`,
   `AppleHIDMouseEventDriver`) probes for vendor-specific match
   dictionaries before falling back to the generic
   `IOUSBHostHIDDevice` path. Several recovery-only UI panels
   ("Power on Bluetooth Keyboard", multi-touch settings) only appear
   or behave correctly when the chain matches an Apple-VID
   peripheral.

A previous descriptor-only spoof (re-ID'ing usb-kbd as Apple,
withdrawn 2026-05-07) was insufficient: the descriptor-only shape
claimed `AppleUSBTopCaseHIDDriver` at probe score 90000 but failed to
match the vendor-defined HID report descriptor (`UsagePage 0xff00`)
the Apple driver actually expects; the driver then refused to bind
and the device dangled unclaimed.

This series ships two new self-contained USB devices that emit the
real Apple wire format end-to-end: byte-identical descriptor strings,
HID report descriptors, endpoint topology, and (for the keyboard)
1 Hz vendor heartbeat.

## What's sent

Two-patch series + cover letter, generated against qemu.git v11.0.0.

| Patch | Subject |
|-------|---------|
| 0/2 | cover letter |
| 1/2 | `hw/usb/dev-hid: add apple-magic-keyboard` |
| 2/2 | `hw/usb/dev-hid: add apple-mighty-mouse` (with `apple-magic-tablet` legacy alias) |

### Patch 1 — apple-magic-keyboard

PID 0x026c, idVendor 0x05ac (Apple), bcdDevice 0x0870. Composite
device with two HID interfaces.

- **Interface 0 — Apple-vendor HID (`UsagePage 0xff00`).** Three
  vendor input report IDs (`0xe0` keyboard event, `0x9a` modifier
  signal, `0x90` power/battery status) and an Apple-vendor descriptor
  byte-identical to a real Magic Keyboard with Numeric Keypad. ACKs
  feature-report and GET_REPORT polls so the driver's match phase
  doesn't enter a retry storm. Emits a 1 Hz `0x90` battery heartbeat
  (charging on AC, 100%) so the userspace HID watchdog considers the
  device alive.

- **Interface 1 — standard HID Boot Keyboard (`UsagePage 0x07`,
  bInterfaceSubClass 1, bInterfaceProtocol 1).** Boot-keyboard report
  descriptor (10-byte input report — modifier + reserved + 6 keycodes
  + Consumer Eject + Vendor 0xff). EP2 IN. Wired to QEMU's input
  subsystem via `qemu_input_handler_register`; any RFB / SPICE / SDL /
  HMP `sendkey` source drives it.

`bcdUSB = 0x0200` advertised but the configuration is duplicated
across `.full` and `.high` USBDesc speed slots — without `.full`,
QEMU's USB stack crashes at enumeration on full-speed (12 Mb/s)
busses, which is the speed the real device negotiates.

### Patch 2 — apple-mighty-mouse

PID 0x0304, idVendor 0x05ac, bcdDevice 0x0150. Apple Mighty Mouse —
the wired ball mouse (M9087, 2005). Single HID interface, single IN
endpoint, standard boot mouse + scroll Report Descriptor:

| byte | content |
|------|---------|
| 0 | 3-bit button mask (left, right, middle) + 5-bit padding |
| 1 | signed int8 dX |
| 2 | signed int8 dY |
| 3 | signed int8 vertical wheel (Generic Desktop / Wheel) |
| 4 | signed int8 horizontal wheel (Consumer / AC Pan — Mighty Mouse scroll-ball X axis) |

`bSubClass=1, bProto=2` (boot mouse) — guaranteed-binding driver
match across every macOS version. Binds
`AppleHIDMouseEventDriver` / `IOHIDPointing` natively; no
proprietary vendor protocol required.

Wired to QEMU's input subsystem accepting REL motion, ABS motion, BTN,
and wheel events. ABS events (from VNC) are converted to per-frame
REL deltas via a `last_abs_x/y` tracker since the boot-mouse report
only carries int8 dX/dY. A bounded queue (depth 64) drains
accumulated motion across multiple reports per input sync so cursor
movement stays responsive under large pointer deltas.

A legacy TypeInfo alias `apple-magic-tablet` is registered alongside
the canonical type so existing in-tree downstream consumers (e.g.
mos-docker test scripts using `TABLET_DEVICE=apple-magic-tablet`)
keep working during the transition. The alias has no permanent place
upstream and can be dropped once consumers move to
`apple-mighty-mouse`.

## Apple-vendor identity rationale

Both devices declare idVendor `0x05ac` and the real PIDs (`0x026c`,
`0x0304`). Precedent for Apple-aware QEMU devices that ship the real
identity:

- `isa-applesmc` — ships the verbatim Apple OSK string and
  emulates the Apple SMC PMIO protocol.
- `mac99` machine type — emulates real Apple PowerPC hardware.
- The previously-merged `apple-gfx.m` (Phil Dennis-Jordan, 2024)
  — emulates the host-Apple paravirtualised GPU.

These devices exist for the macOS-guest fidelity use case; without
them, macOS guests under QEMU/KVM exhibit a long tail of degraded
behaviours (Setup Assistant wizards, retry storms, "no recognised
device" UI). The Apple PnP IDs are public identifiers (linux-usb's
`usb.ids` database) and emulating them does not by itself bypass any
Apple licensing check beyond what `isa-applesmc` already requires.

If the upstream maintainer prefers, the Apple PID/VID can be gated
behind an `apple-id=on/off` device property (default off, generic QEMU
IDs otherwise) — a v2 is prepared addressing that review style. The
current submission carries the Apple IDs unconditionally, matching the
precedent set by sibling devices.

## Measured impact

Visual proof captured against macOS 15.7.5 + qemu-system-x86_64 +
qemu-xhci + isa-applesmc + apple-magic-keyboard + apple-mighty-mouse:

| Behaviour | usb-kbd / usb-tablet | apple-magic-keyboard / apple-mighty-mouse |
|---|---|---|
| First-boot Keyboard Setup Assistant | shows; blocks 3-5 min | does not show |
| Recovery "Power on Bluetooth Keyboard" UI | shows transiently | does not show |
| HID keyboard driver bind | `IOUSBHostHIDDevice` (generic, score ~10000) | `AppleUSBTopCaseHIDDriver` → `AppleDeviceManagementHIDEventService` → `AppleHIDKeyboardEventDriverV2` (score 90000) |
| HID mouse driver bind | `IOUSBHostHIDDevice` (generic) | `AppleHIDMouseEventDriver` / `IOHIDPointing` (Apple-VID match) |
| HID delivery proven via input → screendump diff | n/a | `./mos verify 3` Tier B PASS (boot diff + post-input diff against gold) |

## What's not in this series

- Magic Trackpad 2 multitouch (`PID 0x0265`) — gated behind a
  ~1388-byte vendor multitouch frame (Report 0x44) the wire bytes of
  which we don't have ground-truth for. A follow-up series adds it
  once the vendor-multitouch wire format is captured cleanly. See
  the cover letter "Why Mighty Mouse, not Magic Trackpad" section.
- Magic Mouse 2 / Magic Mouse (`PID 0x030d` / `0x030e`) — different
  PID family with optical-sensor + multi-touch top, defer.
- Bluetooth-mode emulation — the BT face routes input events
  through macOS's HID Event System before they reach
  `IOHIDManager`, so the cooked event path is the only thing that
  sees them. The USB capture is the authoritative wire format and
  is what we emulate.
- JIS / ISO / fn-remap quirks — US ANSI layout only for v1.
- Touch ID / fingerprint sensor (different PID family) — out of
  scope.
