# hw/usb/dev-hid: add apple-kbd / apple-mouse / apple-tablet wrappers

## Summary

Adds three opt-in QOM types that inherit from the existing
`usb-kbd`, `usb-mouse`, and `usb-tablet` HID devices and override
only the USB enumeration-time descriptor (vendor 0x05ac, real Apple
product IDs, Apple Inc. manufacturer / product strings). The HID
report descriptor and data-handling paths are reused 1:1 from the
parent devices.

| Device        | Parent      | VID    | PID    | Product string         |
|---------------|-------------|--------|--------|------------------------|
| apple-kbd     | usb-kbd     | 0x05ac | 0x024f | "Apple Keyboard"       |
| apple-mouse   | usb-mouse   | 0x05ac | 0x030d | "Apple Mighty Mouse"   |
| apple-tablet  | usb-tablet  | 0x05ac | 0x030e | "Apple Magic Trackpad" |

## Motivation

macOS guests run the Keyboard Setup Assistant on first boot for any
HID keyboard whose USB descriptor does not identify it as
Apple-manufactured. On a freshly installed macOS 15 VM this
introduces a 3-5 minute delay before the desktop becomes usable,
and on headless VNC/SPICE installs with no pointer input the boot
hangs indefinitely (the assistant blocks on user interaction).

Same-layer fix is preferred: the guest must see Apple-identity USB
HID devices at descriptor enumeration time, before any userland
boot code runs. A post-boot launchd / configuration-profile
workaround is not acceptable here because the assistant runs before
any such workaround has a chance to execute.

## Zero behaviour change for existing users

No existing device's default identity, vmstate, or property set is
touched. `-device usb-kbd`, `-device usb-mouse`, and `-device
usb-tablet` are byte-for-byte unchanged. Existing VMs migrate
across this change without state compatibility impact.

## Why three new devices rather than a property

A `-device usb-kbd,vendorid=0x05ac,productid=0x024f` property-set
approach was considered. The wrapper-device shape was chosen
because:

 * It also lets the product-description string, manufacturer
   string, serial number, and device-release bcdDevice all change
   atomically with the IDs - a property-driven approach would need
   roughly half a dozen properties to cover the full descriptor.
 * Apple IDs are a well-known constellation with a well-understood
   use case (macOS guest compatibility), so giving them a named
   device makes command lines self-documenting:
   `-device apple-kbd` reads as obvious intent.
 * Migration compatibility is cleanly per-type.

## Testing

 * macOS 15.7.5 guest with `-device apple-kbd,bus=xhci.0` +
   `-device apple-tablet,bus=xhci.0`: first-boot Keyboard Setup
   Assistant is skipped, boot proceeds directly to login.
   Inside the guest, `ioreg -c IOUSBHostDevice` reports
   "Apple Inc." for `USB Vendor Name` on the HID devices.
 * macOS 15.7.5 guest with `-device usb-kbd,bus=xhci.0` +
   `-device usb-tablet,bus=xhci.0`: Keyboard Setup Assistant
   appears (baseline behaviour preserved).
 * Linux guest (Fedora 40) with `-device apple-kbd`: `lsusb`
   reports `ID 05ac:024f Apple, Inc.`; input works identically
   to `-device usb-kbd`.
 * Linux guest (Fedora 40) with `-device usb-kbd`: unchanged
   from pre-series behaviour.
