# vmware_vga: raise resolution cap, advertise modern capability bits

The QEMU `vmware_vga` device reports `SVGA_CAP_NONE` when the
guest reads `SVGA_REG_CAPABILITIES`, and caps mode validation
at 2368x1770. This was historically safe because the device
implements almost none of the modern VMware SVGA II feature
set, but the machinery for five capability bits is in fact
already present - the device just never told the guest about
them.

This four-patch series fixes that. Each patch touches a
single feature and is independently revertible.

| Patch | Subject |
|-------|---------|
| 1/4   | `hw/display/vmware_vga: raise resolution ceiling to 5K (5120x2880)` |
| 2/4   | `hw/display/vmware_vga: advertise PITCHLOCK capability` |
| 3/4   | `hw/display/vmware_vga: advertise EXTENDED_FIFO capability` |
| 4/4   | `hw/display/vmware_vga: advertise 8BIT_EMULATION, ALPHA_BLEND and ALPHA_CURSOR` |

## Motivation

Modern VMware SVGA II guest drivers (Linux vmwgfx, macOS
10.13+, Windows 10+) refuse to take optional fast paths when
the corresponding capability bits are not advertised. The
effect on macOS 15 guests in particular is:

  * Cursor is monochrome and flickers (ALPHA_CURSOR missing).
  * Scanout pitch does not align to guest framebuffer on
    non-power-of-two widths (PITCHLOCK missing).
  * Resolution ceiling is ~1440p regardless of VRAM.

Advertising the bits is a zero-risk change: the underlying
command dispatchers already handle the corresponding FIFO
commands; the guest is merely told it may use them.

## Dependencies

None. Patches are ordered for readability (geometry first,
then progressively richer capability bits) but are
independent.

## Testing

See `TESTING.md` for the full recipe. In short: the advertised
capability mask changes from 0 to 0x2a280 (plus any
HW_*_ACCEL bits the build sets). Linux vmwgfx `dmesg`
stops logging "device lacks extended FIFO" warnings; macOS
15 vmware_vga driver switches from the monochrome cursor
path to the ARGB cursor path.
