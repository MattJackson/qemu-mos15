# Testing the vmware_vga capability series

## Prerequisites

* QEMU built with `--target-list=x86_64-softmmu --enable-kvm`
  and `CONFIG_VMWARE_VGA=y` (on by default).
* A Linux guest with the `vmwgfx` driver module and/or a
  macOS 15 guest configured to use the `vmware_vga`
  framebuffer driver.

## Capability register read-back

Boot a guest with the device:

```
qemu-system-x86_64 -machine q35,accel=kvm -cpu host \
    -smp 4 -m 4096 \
    -vga vmware \
    -drive if=virtio,file=guest.qcow2
```

On Linux, `xrandr --verbose` and `dmesg | grep vmwgfx` should
show:

```
[    3.214567] [drm] Device: vmwgfx
[    3.214589] [drm] capabilities: 0x00000000 00042a80
[    3.214602] [drm] FIFO capabilities: extended
```

(The precise bitmask depends on which HW_*_ACCEL macros were
enabled at build time, but bits 8, 9, 13, 15 and 17 should
all be set.)

Without the series:

```
[    3.214567] [drm] Device: vmwgfx
[    3.214589] [drm] capabilities: 0x00000000 00000000
[    3.214602] [drm] FIFO capabilities: basic
```

## Mode-validation ceiling

```
xrandr --output Virtual-1 --mode 3840x2160
# Before series: fails with "invalid mode"
# After  series: succeeds (up to VRAM-sizing limit)
```

## macOS 15 cursor path

Open Console.app, filter for `com.apple.driver.AppleVMwareDisplay`,
then move the mouse cursor:

```
Before series: "using legacy monochrome cursor"
After  series: "using ALPHA_CURSOR fast path"
```

## Regression check

No behaviour change is expected on the existing supported
resolutions (up to 1770p) or on guests that do not read
`SVGA_REG_CAPABILITIES`. The existing QEMU test harness in
`tests/qtest/vmware-vga-test.c` (if present in your tree)
must continue to pass.
