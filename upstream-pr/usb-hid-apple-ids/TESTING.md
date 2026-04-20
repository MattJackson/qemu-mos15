# Testing the usb-hid Apple-ID patch

## Prerequisites

* QEMU built with `--target-list=x86_64-softmmu --enable-kvm`.
* A macOS 15.x guest image.

## Reproduce the regression (unpatched)

```
qemu-system-x86_64 -machine q35,accel=kvm -cpu host \
    -smp 4 -m 4096 \
    -usb \
    -device usb-kbd \
    -device usb-tablet \
    -drive if=virtio,file=macos.qcow2 \
    -device isa-applesmc,osk='<osk>'
```

On first boot, macOS will display the "Set Up Your
Keyboard" assistant. On headless VNC/spice sessions with no
mouse input this hangs indefinitely; with input it takes
3-5 minutes of clicks to complete.

## Verify the fix

Apply the patch, rebuild, boot the same guest with the same
command line. The keyboard setup assistant is skipped; boot
continues directly to the login screen.

## Linux/Windows regression check

Boot a Linux or Windows guest. `lsusb` (Linux) or Device
Manager (Windows) now shows the HID devices as Apple devices
with the new vendor/product IDs, but there is no functional
change - the HID report descriptors are unchanged, so input
works identically.

## User-visible side effect

Any guest OS that special-cases Apple USB peripherals (macOS
for power management, iOS host tools for USB debugging, some
boot loaders that bind to Apple VID on first-found HID) will
now see the Apple IDs where it previously saw QEMU's
`0x0627`. This is the reason this patch is marked as a draft
and is expected to be converted to a device property before
final merge.
