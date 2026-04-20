# Build Integration: apple-gfx-pci-linux in QEMU

## Overview

This document specifies how the Linux port of the Apple ParavirtualizedGraphics PCI device integrates into QEMU's build pipeline via the mos-qemu partial fork and docker-macos container image.

## Files in mos-qemu

The following files in `/Users/mjackson/qemu-mos15/` are overlay/replacement files for QEMU 10.2.2:

- `hw/display/apple-gfx-pci-linux.c` — Main device implementation
- `hw/display/apple-gfx-common-linux.c` — Shared utilities
- `hw/display/apple-gfx-linux.h` — Public header
- `hw/display/meson.build` — QEMU build config (REPLACES upstream file, adds our device)
- `hw/display/Kconfig` — Device selection config (REPLACES upstream file, adds our device)
- `hw/display/BUILD-apple-gfx-pci-linux.md` — This file
- `pc-bios/meson.build` — QEMU pc-bios install list (REPLACES upstream; adds `apple-gfx-pci.rom`)
- `pc-bios/apple-gfx-pci.rom` — Apple's extracted AppleParavirtEFI.rom (16.5 KB);
  default option ROM for `apple-gfx-pci`. Source-of-record lives at
  `mos/paravirt-re/option-rom/AppleParavirtEFI.rom`. Phase 5.X replaces this
  with an in-tree EDK2 build.

## Build-time copy pattern

The `docker-macos/Dockerfile` downloads full QEMU 10.2.2 upstream, then overlays our changes via `cp` commands. The new copy block must be:

```dockerfile
cd /tmp/qemu-${QEMU_VERSION} && \
cp /tmp/qemu-mos15-main/hw/display/apple-gfx-pci-linux.c hw/display/ && \
cp /tmp/qemu-mos15-main/hw/display/apple-gfx-common-linux.c hw/display/ && \
cp /tmp/qemu-mos15-main/hw/display/apple-gfx-linux.h hw/display/ && \
cp /tmp/qemu-mos15-main/hw/display/meson.build hw/display/meson.build && \
cp /tmp/qemu-mos15-main/hw/display/Kconfig hw/display/Kconfig && \
cp /tmp/qemu-mos15-main/pc-bios/meson.build pc-bios/meson.build && \
cp /tmp/qemu-mos15-main/pc-bios/apple-gfx-pci.rom pc-bios/apple-gfx-pci.rom
```

This follows the existing pattern for `applesmc.c`, `vmware_vga.c`, and `dev-hid.c`.

## Build configuration

### meson.build changes

In `/Users/mjackson/qemu-mos15/hw/display/meson.build`:

```meson
# Apple ParavirtualizedGraphics PCI device (Linux C port).
# Declared self-contained here so we do not need to overlay root meson.build.
if config_all_devices.has_key('CONFIG_APPLE_GFX_PCI_LINUX')
  libapplegfx_vulkan = dependency('libapplegfx-vulkan', required: false)
  if libapplegfx_vulkan.found()
    applegfx_ss = ss.source_set()
    applegfx_ss.add(when: 'CONFIG_APPLE_GFX_PCI_LINUX', if_true: [
      files('apple-gfx-pci-linux.c', 'apple-gfx-common-linux.c'),
      libapplegfx_vulkan,
    ])
    hw_display_modules += {'apple_gfx_pci_linux': applegfx_ss}
  endif
endif
```

The dependency is declared inline rather than at root `meson.build` so we
avoid overlaying a huge upstream file. `required: false` means missing
libapplegfx-vulkan silently drops the device.

### Kconfig changes (8 lines added)

Lines 154-161 in `/Users/mjackson/qemu-mos15/hw/display/Kconfig`:

```kconfig
config APPLE_GFX_PCI_LINUX
    bool "Apple ParavirtualizedGraphics PCI device (Linux)"
    default y if PCI_DEVICES
    depends on PCI
    help
      Enable the Linux-compatible Apple ParavirtualizedGraphics PCI
      device. Requires libapplegfx-vulkan library with lavapipe Vulkan
      backend. The runtime dependency is gated at the meson level —
      if libapplegfx-vulkan is not found via pkg-config, the device
      source files are dropped from the build even if this symbol is y.
```

Note: we intentionally do NOT add `depends on LIBAPPLEGFX_VULKAN` because
the matching proxy symbol would have to live in `Kconfig.host`, which would
require yet another overlay file. The meson-level gate is sufficient.

## Dockerfile changes required

The builder stage must:

1. Install Vulkan runtime + development deps
2. Clone and build `libapplegfx-vulkan` from source
3. Install libapplegfx-vulkan to system (`.pc` discoverable by pkg-config)
4. Copy our modified `meson.build` + `Kconfig` files
5. Run QEMU configure WITHOUT additional flags (meson auto-detects via pkg-config)

### Proposed Dockerfile block (~20 lines)

Insert BEFORE the `./configure` step:

```dockerfile
# Vulkan + Mesa lavapipe for libapplegfx-vulkan
RUN apk add --no-cache \
    mesa-vulkan-swrast \
    vulkan-loader vulkan-headers \
    vulkan-tools

# Clone and build libapplegfx-vulkan
RUN git clone https://github.com/MattJackson/libapplegfx-vulkan.git /tmp/libapplegfx-vulkan \
    && cd /tmp/libapplegfx-vulkan \
    && meson setup --prefix=/usr --libdir=lib builddir \
    && ninja -C builddir install

# Copy our overlay files + new source files
RUN cd /tmp/qemu-${QEMU_VERSION} && \
    cp /tmp/qemu-mos15-main/hw/display/apple-gfx-pci-linux.c hw/display/ && \
    cp /tmp/qemu-mos15-main/hw/display/apple-gfx-common-linux.c hw/display/ && \
    cp /tmp/qemu-mos15-main/hw/display/apple-gfx-linux.h hw/display/ && \
    cp /tmp/qemu-mos15-main/hw/display/meson.build hw/display/meson.build && \
    cp /tmp/qemu-mos15-main/hw/display/Kconfig hw/display/Kconfig && \
    cp /tmp/qemu-mos15-main/pc-bios/meson.build pc-bios/meson.build && \
    cp /tmp/qemu-mos15-main/pc-bios/apple-gfx-pci.rom pc-bios/apple-gfx-pci.rom
```

### Runtime stage changes (~5 lines)

Modify final Alpine `apk add` to include Vulkan runtime:

```dockerfile
RUN apk add --no-cache \
    glib pixman libcap-ng libseccomp libslirp \
    libaio libbz2 dtc bash iproute2 ovmf \
    vulkan-loader mesa-vulkan-swrast
```

## Verification

After configure, check that the device is recognized:

```bash
# In container during build
cd /tmp/qemu-10.2.2/build && \
meson introspect --targets | grep apple_gfx_pci_linux
```

Or check the config output:

```bash
cd /tmp/qemu-10.2.2/build && \
grep -i apple_gfx_pci_linux meson-logs/meson-log.txt
```

If libapplegfx-vulkan is **not** found, the device silently does not build (feature: 'auto'). Check:

```bash
pkg-config --cflags --libs libapplegfx-vulkan
```

## Build-chicken-egg issues

**None identified.** The dependency chain is clean:

1. Vulkan headers + tools installed by apk (no build needed)
2. libapplegfx-vulkan built and installed to `/usr` before QEMU configure
3. meson's `pkg-config` discovers it automatically
4. If missing, QEMU configure skips the device (auto feature)

## Testing the device in QEMU

Once the image builds, verify the device is available:

```bash
qemu-system-x86_64 -device help | grep -i apple
```

Should list: `apple-gfx-pci-linux`

To instantiate:

```bash
qemu-system-x86_64 \
  -device apple-gfx-pci-linux,id=gpu0 \
  -m 4G \
  ...other args...
```

## Upstreaming strategy

Post-Phase 3, submit to qemu-devel as a patch series:
1. meson/Kconfig feature flag + dependency
2. device source files
3. documentation

The 'auto' feature + pkg-config discovery ensures upstream compatibility.
