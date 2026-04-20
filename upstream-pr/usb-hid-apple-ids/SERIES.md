# hw/usb/dev-hid: add apple-kbd / apple-mouse / apple-tablet wrappers

Three-patch series that adds opt-in HID wrapper devices with Apple
Inc. vendor/product/string descriptors, inheriting from upstream's
`usb-kbd`, `usb-mouse`, and `usb-tablet` respectively.

## Patches

 * 0001 - add `apple-kbd` (parent `usb-kbd`, VID:PID `0x05ac:0x024f`)
 * 0002 - add `apple-mouse` (parent `usb-mouse`, VID:PID `0x05ac:0x030d`)
 * 0003 - add `apple-tablet` (parent `usb-tablet`, VID:PID `0x05ac:0x030e`)

## Design

Each new device registers a `TypeInfo` whose `.parent` is the
corresponding existing HID device. The class_init overrides only
`USBDeviceClass::realize` and `USBDeviceClass::product_desc`. The
realize function wires in an Apple-specific `USBDesc` / `USBDescDevice`
/ `USBDescStrings` set and then delegates to the same internal
`usb_hid_initfn()` helper the parent devices use, so the HID report
descriptor, endpoint topology, vmstate, device properties, and data
paths are reused 1:1. The only wire-visible change is the
enumeration-time descriptor: vendor/product IDs, manufacturer string,
product string, and serial number.

The device ID set reflects real, shipping Apple HID peripherals:

| Device        | VID    | PID    | Product string         |
|---------------|--------|--------|------------------------|
| apple-kbd     | 0x05ac | 0x024f | "Apple Keyboard"       |
| apple-mouse   | 0x05ac | 0x030d | "Apple Mighty Mouse"   |
| apple-tablet  | 0x05ac | 0x030e | "Apple Magic Trackpad" |

## Motivation

macOS guests run the Keyboard Setup Assistant on first boot for any
HID device whose USB descriptor does not identify it as
Apple-manufactured. On a freshly installed macOS 15 VM this produces
a multi-minute delay before the desktop becomes usable; on headless
VNC/SPICE installs with no pointer input the boot hangs
indefinitely, because the assistant demands user input before
allowing the first-login flow to proceed.

This series makes the Apple-identity behaviour available as explicit
opt-in devices so macOS-guest users can select them without
affecting any existing QEMU command line or other guest OS.

## Zero behaviour change for existing users

No upstream device's default identity, vmstate, or property set is
touched. `-device usb-kbd`, `-device usb-mouse`, and `-device
usb-tablet` all behave exactly as they did before this series.

## Status

**Ready to submit.**

This supersedes an earlier draft that re-ID'd the base `usb-kbd` /
`usb-mouse` / `usb-tablet` devices globally. Per expected upstream
review feedback, that approach was rejected as affecting all guests
(not just macOS) and has been replaced by the opt-in wrapper device
design in this series.
