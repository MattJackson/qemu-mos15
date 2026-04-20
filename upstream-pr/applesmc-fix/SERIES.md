# applesmc: fix GET_KEY_BY_INDEX iteration and populate boot keys

The QEMU applesmc device implements just enough of the Apple
SMC PMIO protocol to satisfy the OSK boot check on older macOS
versions. On modern macOS guests (x86 10.14+, all of the 15.x
series) the real AppleSMC kext enumerates the SMC key space at
boot via GET_KEY_BY_INDEX_CMD (0x12). The current device only
acknowledges READ_CMD (0x10) at the command port; every other
command falls through to the default arm of the switch and
sets ST_1E_BAD_CMD.

The macOS driver interprets the resulting 0x82 reply as
"spurious data" and enters a retry loop that floods the kernel
log with kSMCSpuriousData (0x81) / kSMCKeyNotFound errors at
roughly 1800 events per second, pegging `kernel_task` at ~70%
CPU and `WindowServer` at ~509% CPU. This reproduces reliably
on any recent macOS 15 guest booted with `-device
isa-applesmc,osk=<valid-OSK>`.

This four-patch series fixes the bug and rounds out the
key set to what modern macOS expects at boot.

| Patch | Subject |
|-------|---------|
| 1/4   | `hw/misc/applesmc: fix GET_KEY_BY_INDEX to return real keys, accept WRITE/TYPE commands` |
| 2/4   | `hw/misc/applesmc: add #KEY total-count key` |
| 3/4   | `hw/misc/applesmc: replace zero sensor defaults with realistic iMac20,1 values` |
| 4/4   | `hw/misc/applesmc: add boot-required keys HE0N/HE2N/WDTC and platform identity` |

## Measured impact (macOS 15.7.5 guest, iMac20,1 profile)

| Metric              | Before     | After    |
|---------------------|-----------:|---------:|
| SMC errors / 5 s    |     9,225  |      2   |
| `kernel_task` CPU   |     70 %   |    ~2 %  |
| `WindowServer` CPU  |    509 %   |    ~6 %  |

## Dependencies

None. Each patch is independent but they are best applied in
order: 1 is the bug fix, 2 reduces boot-log noise, 3 silences
sensor-poll retry loops, 4 plugs the remaining holes.

## Audience

`hw/misc/` maintainers and anyone running macOS guests on
QEMU/KVM x86_64. The fix is fully backwards compatible: the
only visible guest-side change is that GET_KEY_BY_INDEX_CMD
now returns real key names instead of a stream of zeros.
