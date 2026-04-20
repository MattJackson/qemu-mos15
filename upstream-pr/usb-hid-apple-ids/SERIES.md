# usb-hid: advertise Apple vendor ID and strings

Single-patch package. Re-IDs the QEMU USB HID devices
(mouse, tablet, keyboard) so they identify as Apple Magic
peripherals. On macOS guests this skips the 3-5 minute
Keyboard Setup Assistant on first boot.

## Upstream-acceptance note

This patch changes the USB descriptor identity of all
`usb-mouse` / `usb-tablet` / `usb-kbd` devices, not just
those attached to macOS guests. That is almost certainly
not what upstream maintainers want.

The reasonable upstream conversion is to add a
`vendor-string=` / `vendorid=` property to the device that
defaults to the existing QEMU identity. Users who want the
Apple IDs (or any other vendor IDs) pass the property at
device creation time.

This package is submitted as the minimal change for
discussion; expect reviewer feedback to request conversion
to a property-driven approach before merge. The work to do
that conversion is straightforward and is likely to land
as the final form of the patch.

## Status

Draft / discussion; not ready to merge in its current form
but the discussion is worth opening so macOS-guest users
have a path forward.
