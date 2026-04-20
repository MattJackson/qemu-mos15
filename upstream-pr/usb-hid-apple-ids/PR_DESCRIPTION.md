# hw/usb/dev-hid: use Apple vendor ID and strings for HID devices

## Summary

macOS guests run the Keyboard Setup Assistant at first boot
for any HID keyboard/mouse/trackpad whose USB descriptor
does not identify it as Apple-manufactured. On a freshly-
installed macOS 15 VM this introduces a 3-5 minute delay
before the desktop becomes usable, and on headless VMs it
hangs the boot indefinitely.

Re-ID the three HID device descriptors to use Apple's vendor
ID 0x05ac and product ID 0x0267 (Magic Keyboard) and the
string descriptors to "Apple Inc." / "Magic Mouse" / "Magic
Trackpad" / "Magic Keyboard".

## Known limitation

This change is global: `-device usb-mouse` now identifies as
an Apple mouse on all guests, not just macOS. The preferred
upstream shape is almost certainly a `vendor-string=` / 
`vendorid=` property on the device, defaulting to the
existing QEMU identity. This PR is opened to start that
discussion; I am happy to convert to a property in a
follow-up revision once the shape is agreed.

## Testing

macOS 15.7.5 guest: before this patch, first-boot shows the
Keyboard Setup Assistant; after, boot proceeds to the desktop
without assistant interaction. No change observed on Linux
or Windows guests - `lsusb` reports the new IDs but the
operating systems have no special behaviour for them.
