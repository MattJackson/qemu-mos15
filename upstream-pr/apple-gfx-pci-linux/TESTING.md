# Testing the apple-gfx-pci-linux port

## Prerequisites

* Linux host with KVM, Mesa >= 23.0 (lavapipe driver), and a
  graphics stack that can be driven from a CPU rasteriser.
* `libapplegfx-vulkan` built and installed (see note below).
* A macOS 15.x guest image configured to use Apple's
  `AppleParavirtGPU.kext` (the default on aarch64; on x86
  the kext must be present and matched).

## Building the library

Until `libapplegfx-vulkan` is packaged, build it from source:

```bash
git clone https://example.invalid/libapplegfx-vulkan.git
cd libapplegfx-vulkan
meson setup build --prefix=/usr
ninja -C build
sudo ninja -C build install
```

(Substitute the real library repository URL; the reference
implementation used to develop this patch series is
maintained out-of-tree pending upstream packaging.)

Verify `pkg-config --exists libapplegfx-vulkan` returns 0.

## Building QEMU with the device

```bash
./configure --target-list=x86_64-softmmu --enable-kvm
make -j$(nproc) qemu-system-x86_64
```

Check the Kconfig symbol was picked up:

```bash
grep APPLE_GFX_PCI_LINUX build/config-host.h
# Expected: #define CONFIG_APPLE_GFX_PCI_LINUX 1
```

And that the library was found:

```bash
./build/qemu-system-x86_64 -device help 2>&1 | grep apple-gfx-pci
# Expected: apple-gfx-pci, bus PCI, ...
```

## Running a guest

```bash
qemu-system-x86_64 -machine q35,accel=kvm -cpu host \
    -m 8G -smp 4 \
    -device apple-gfx-pci,gpu_cores=8 \
    -drive if=virtio,file=macos15.qcow2 \
    -device isa-applesmc,osk='<osk>' \
    ...
```

The option ROM is loaded automatically from
`$prefix/share/qemu/apple-gfx-pci.rom`.

## Expected behaviour

First-boot kernel log should show the guest kext attaching:

```
AppleParavirtGPU: IOPCIDevice attached (vendor 0x106b
    device 0x1b30)
AppleParavirtGPU: option ROM present
AppleParavirtGPU: entering para-virt mode
```

The QEMU host log (`-d trace:apple_gfx_*`) shows the
expected sequence:

```
apple_gfx_common_init apple-gfx-linux 16384
apple_gfx_create_task vm_size=... base=...
apple_gfx_map_memory task=... count=... offset=...
apple_gfx_new_frame
```

## Regression check

On a host without `libapplegfx-vulkan` installed, the meson
gate should drop the device from the build silently, and
`-device help` should not list `apple-gfx-pci-linux`. The
build must succeed regardless of library presence.
