# Testing: apple-kbd / apple-mouse / apple-tablet wrappers

## Prerequisites

* QEMU built with `--target-list=x86_64-softmmu --enable-kvm`
  (`CONFIG_USB_HID=y` - enabled by default).
* A macOS 15.x guest image and OpenCore bootloader image.

## Reproduce the regression (upstream usb-kbd / usb-mouse / usb-tablet)

```
qemu-system-x86_64 -machine q35,accel=kvm -cpu host \
    -smp 4 -m 4096 \
    -device qemu-xhci,id=xhci \
    -device usb-kbd,bus=xhci.0 \
    -device usb-tablet,bus=xhci.0 \
    -drive if=virtio,file=macos.qcow2 \
    -device isa-applesmc,osk='<osk>'
```

On first boot, macOS displays "Set Up Your Keyboard". On headless
VNC/SPICE sessions with no pointer input this hangs indefinitely;
with input it takes several minutes of clicks to complete.

## Verify the fix

Swap `usb-kbd` / `usb-tablet` for the new wrapper devices:

```
qemu-system-x86_64 -machine q35,accel=kvm -cpu host \
    -smp 4 -m 4096 \
    -device qemu-xhci,id=xhci \
    -device apple-kbd,bus=xhci.0 \
    -device apple-tablet,bus=xhci.0 \
    -drive if=virtio,file=macos.qcow2 \
    -device isa-applesmc,osk='<osk>'
```

The Keyboard Setup Assistant is skipped; boot proceeds directly
to the login screen.

Inside the guest, verify the descriptor identity:

```
# macOS
ioreg -c IOUSBHostDevice -l | grep -E 'idVendor|idProduct|USB Product Name'
# Expected:
#   "USB Vendor Name" = "Apple Inc."
#   "idVendor" = 1452   # 0x05ac
#   "idProduct" = 591   # 0x024f for apple-kbd
#   "idProduct" = 782   # 0x030e for apple-tablet
#   "USB Product Name" = "Apple Keyboard" / "Apple Magic Trackpad"
```

## No-regression check (existing guests)

With `-device usb-kbd` / `-device usb-mouse` / `-device usb-tablet`
(the upstream defaults), Linux and Windows guests report identical
`lsusb` / Device Manager output before and after this series. No
migration-stream compatibility breakage, since the existing device
types' vmstate is untouched.

## Cross-guest check (Linux with apple-kbd)

Booting a Linux guest with `-device apple-kbd` shows the new
descriptor in `lsusb`:

```
Bus 001 Device 00X: ID 05ac:024f Apple, Inc.
```

Input handling is identical to `-device usb-kbd` (the HID report
descriptor is unchanged).

## Absolute-pointing interaction note

`apple-tablet` inherits the absolute-position input pipeline from
`usb-tablet`. The Apple real-hardware trackpad (product 0x030e) is
itself an absolute-pointing device in macOS's HID stack, so the
emulated behaviour is consistent with what the identity advertises.
No special-case handling is required inside macOS.
