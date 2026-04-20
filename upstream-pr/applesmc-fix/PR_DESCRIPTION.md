# hw/misc/applesmc: fix GET_KEY_BY_INDEX iteration, populate boot keys

## Summary

The QEMU `applesmc` ISA device implements just enough of the
Apple SMC PMIO protocol to satisfy the OSK boot check on older
macOS versions. On modern macOS guests (x86 10.14+; all 15.x),
the AppleSMC kext enumerates the SMC key space at boot via
`APPLESMC_GET_KEY_BY_INDEX_CMD` (0x12). The current device
only acknowledges `APPLESMC_READ_CMD` (0x10) at the command
port, so the indexed-iteration command falls through to the
default arm of the switch and sets `ST_1E_BAD_CMD`.

macOS interprets the resulting 0x82 error as a device wedge
and enters a retry loop that emits `kSMCSpuriousData` /
`kSMCKeyNotFound` errors at ~1800 events/sec, pegging
`kernel_task` and `WindowServer` at 70% and 509% CPU
respectively on a Sequoia 15.7.5 guest.

## Patch series

1. `hw/misc/applesmc: fix GET_KEY_BY_INDEX to return real keys,
   accept WRITE/TYPE commands` - implements the indexed
   iteration, accepts WRITE/TYPE at the command port, and
   returns zeroed payloads (logged at LOG_UNIMP) instead of
   NOEXIST for unknown keys.
2. `hw/misc/applesmc: add #KEY total-count key` - exposes the
   Apple-convention `#KEY` so the guest knows the iteration
   upper bound.
3. `hw/misc/applesmc: replace zero sensor defaults with
   realistic iMac20,1 values` - populates the sensor table
   so the AppleSMC client's sensor-poll path does not retry
   at an elevated rate.
4. `hw/misc/applesmc: add boot-required keys HE0N/HE2N/WDTC
   and platform identity` - adds keys read during early boot
   and by graphics power-management init.

## Measured impact

| Metric              | Before     | After    |
|---------------------|-----------:|---------:|
| SMC errors / 5 s    |     9,225  |      2   |
| `kernel_task` CPU   |     70 %   |    ~2 %  |
| `WindowServer` CPU  |    509 %   |    ~6 %  |

## Testing

See `TESTING.md` in the patch set for a full reproduction
recipe (macOS 15.7.5 guest under KVM, 5 seconds of kernel
log). In short: on an unpatched binary the kernel log fills
with `kSMCSpuriousData` errors and `top` shows `kernel_task`
pinned at ~70%. After applying this series the error rate
drops to single digits per 5 s.

## Backwards compatibility

Fully compatible. Guests that do not use GET_KEY_BY_INDEX_CMD
see no change; guests that do now see a walk over the actual
key table instead of a stream of zeros. The new keys are
appended to the existing data_def list, which is already
unordered - no guest relies on a particular ordering.
