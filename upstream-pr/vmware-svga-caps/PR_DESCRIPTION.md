# hw/display/vmware_vga: advertise modern capability bits, raise cap to 5K

## Summary

The `vmware_vga` device currently returns `SVGA_CAP_NONE` for
`SVGA_REG_CAPABILITIES` and caps mode validation at
2368x1770. The underlying device machinery already handles
the FIFO commands and register layout for PITCHLOCK,
EXTENDED_FIFO, 8BIT_EMULATION, ALPHA_BLEND and ALPHA_CURSOR -
only the capability bits are missing. This series advertises
them and raises the mode-validation cap to 5K.

## Patch series

1. `hw/display/vmware_vga: raise resolution ceiling to 5K`
2. `hw/display/vmware_vga: advertise PITCHLOCK capability`
3. `hw/display/vmware_vga: advertise EXTENDED_FIFO capability`
4. `hw/display/vmware_vga: advertise 8BIT_EMULATION, ALPHA_BLEND
   and ALPHA_CURSOR`

## Impact

Per-patch:

| Patch | Guest-visible change |
|-------|----------------------|
| 1/4   | `SVGA_REG_MAX_WIDTH/HEIGHT` go from 2368/1770 to 5120/2880. |
| 2/4   | Linux vmwgfx / macOS vmware_vga use pitchlock to pin scanout pitch across mode changes. |
| 3/4   | Guest uses extended FIFO slots (fence, 3D hwversion, pitchlock) it already had access to. |
| 4/4   | macOS vmware_vga switches to ARGB cursor path; Linux vmwgfx enables alpha-blend uploads. |

No VRAM sizing changes. Guests with the default 16 MB vgamem
still hit VRAM exhaustion at the same point they did before;
this series only changes which layer reports the error.

## Testing

Tested against macOS 15.7.5 and Debian 12 guests. Full
recipe and log-line expectations in `TESTING.md`.

## Backwards compatibility

Fully compatible. Guests that read
`SVGA_REG_CAPABILITIES` and see a larger value than before
are expected to pick richer fast paths. Guests that do not
read the register (there are no real ones - VMware drivers
all do) are unaffected.
