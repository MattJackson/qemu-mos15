# Testing apple-magic-keyboard / apple-mighty-mouse

## Prerequisites

* Linux host with KVM enabled.
* QEMU built with `--target-list=x86_64-softmmu --enable-kvm`,
  including this series.
* A macOS 15.x install image / pre-built guest disk for the
  fidelity-test path (skip to "Linux-guest sanity" otherwise).
* A valid Apple SMC OSK for `isa-applesmc` (required by macOS guests;
  not by the keyboard / mouse themselves).

## Linux-guest sanity (no macOS required)

The descriptors and report descriptors should walk cleanly under
Linux's xHCI + USB-HID stack. Boot any small Linux guest with:

```
qemu-system-x86_64 \
    -enable-kvm -m 2G -smp 2 -machine q35 -cpu host \
    -kernel <vmlinuz> -initrd <initramfs> \
    -append 'console=ttyS0 quiet' \
    -drive file=/path/to/scratch.img,format=raw,if=virtio,snapshot=on \
    -device qemu-xhci,id=xhci \
    -device apple-magic-keyboard,bus=xhci.0 \
    -device apple-mighty-mouse,bus=xhci.0 \
    -display none -serial stdio
```

In the guest:

```
$ lsusb -v -d 05ac:026c   # apple-magic-keyboard
$ lsusb -v -d 05ac:0304   # apple-mighty-mouse
```

Expected:

- `idVendor 0x05ac Apple, Inc.` / `idProduct 0x026c` (or `0x0304`).
- `iManufacturer = "Apple Inc."`,
  `iProduct = "Magic Keyboard with Numeric Keypad"` /
  `"Apple Mighty Mouse"`.
- Keyboard: two interfaces, the second `bInterfaceClass=3`
  `bInterfaceSubClass=1` (Boot) `bInterfaceProtocol=1` (Keyboard).
- Mouse: single interface, `bInterfaceClass=3 bInterfaceSubClass=1
  bInterfaceProtocol=2` (Boot Mouse).
- HID report descriptors decode without complaint.

The legacy alias `apple-magic-tablet` resolves to the same
`apple-mighty-mouse` implementation; both `-device` strings work
during the back-compat window.

## macOS-guest fidelity test

Boot macOS with the new devices in place of `usb-kbd`/`usb-tablet`:

```
qemu-system-x86_64 \
    -enable-kvm -m 8G -smp 4 -machine q35 -cpu host \
    -device 'isa-applesmc,osk=<valid-OSK>' \
    -drive if=pflash,format=raw,readonly=on,file=OVMF_CODE.fd \
    -drive if=pflash,format=raw,file=OVMF_VARS.fd \
    -drive file=opencore.img,format=raw,if=ide \
    -drive file=macos.qcow2,format=raw,if=virtio \
    -device qemu-xhci,id=xhci \
    -device apple-magic-keyboard,bus=xhci.0 \
    -device apple-mighty-mouse,bus=xhci.0 \
    -vnc :0 -qmp unix:/tmp/qmp.sock,server=on,wait=off
```

### HID-stack bind probe (macOS guest)

In the guest, after boot:

```
$ ioreg -c IOUSBHostHIDDevice -l | grep -E 'AppleUSB|AppleHID|VendorID'
```

Expected:

- An `AppleUSBTopCaseHIDDriver` entry under the keyboard's registry
  path (probe score 90000), with
  `AppleDeviceManagementHIDEventService` and
  `AppleHIDKeyboardEventDriverV2` chained beneath.
- An `AppleHIDMouseEventDriver` / `IOHIDPointing` under the mouse's
  registry path (Apple-VID match path, not the generic
  `IOUSBHostHIDDevice` fallback).

### Visible delivery proof

QMP-driven input → screendump diff is the strongest reproducible
proof. Drive a known sequence after boot, take a second screendump,
diff against gold:

```
# socat - UNIX-CONNECT:/tmp/qmp.sock
{"execute":"qmp_capabilities"}
{"execute":"send-key","arguments":{"keys":[{"type":"qcode","data":"a"}]}}
{"execute":"send-key","arguments":{"keys":[{"type":"qcode","data":"b"}]}}
{"execute":"send-key","arguments":{"keys":[{"type":"qcode","data":"c"}]}}
{"execute":"input-send-event","arguments":{"events":[
    {"type":"abs","data":{"axis":"x","value":15000}},
    {"type":"abs","data":{"axis":"y","value":15000}}]}}
{"execute":"screendump","arguments":{"filename":"/tmp/after-input.png"}}
```

At loginwindow, the three keys produce three password dots and the
mouse-move puts the cursor at screen centre. `compare -metric AE`
against a recorded gold image should show ~0 differing pixels (within
fuzz tolerance); a regression in HID delivery surfaces as a
non-trivial diff.

The mos-docker test rig wires this up as `./mos verify 3` (Tier B
input regression) — first run captures the gold, subsequent runs
gold-diff strictly.

## Regression check

`-device usb-kbd` / `-device usb-tablet` must continue to behave
identically to current QEMU (no shared state, no shared vmstate
section). Running both old and new devices simultaneously on the same
xHCI controller must work — they do not collide on bus/port
assignment because each is a separate `USBDevice`.

A migration smoke test: cold-boot guest with apple-magic-keyboard +
apple-mighty-mouse attached, type, then
`migrate exec:gzip > /tmp/m.gz`. The `unmigratable = 1` flag prevents
migration of in-flight HID state across releases (matches the existing
`usb-kbd` behaviour); the device cleanly fails the migrate step rather
than silently corrupting input state. This is intentional; documented
in each device's vmstate descriptor.
