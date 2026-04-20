# Testing the applesmc fix

## Prerequisites

* Linux host with KVM enabled.
* A macOS 15.x (or 14.x) install image or pre-built guest
  disk. macOS 13.x and earlier exhibit the bug more mildly but
  still benefit from the fix.
* QEMU built with `--target-list=x86_64-softmmu --enable-kvm`.

## Reproduce the bug on unpatched QEMU

Boot the guest with a minimal command line that exercises the
applesmc device:

```
qemu-system-x86_64 -machine q35,accel=kvm -cpu host \
    -smp 4 -m 8192 \
    -device isa-applesmc,osk='<real-64-char-OSK>' \
    -drive if=virtio,file=macos.qcow2 \
    ...
```

Once the guest is at the login screen, open Console.app (or
`log stream --predicate 'subsystem == "com.apple.kernel"'`)
and filter for "SMC". You will see a steady torrent of
messages like:

```
kernel: SMC::smcReadKeyAction ERROR: 0x81 (0xb8)
        kSMCSpuriousData: 0xB8 / #KEY / (...)
kernel: AppleSMC::smcReadData ERROR data forbidden 0x81 key
        0x00000000
```

at roughly 1800 events/sec. `top` on the guest shows
`kernel_task` at ~70 % CPU and `WindowServer` at ~509 % CPU
even when the machine is idle.

## Verify the fix

Apply the four-patch series, rebuild, and boot the same
guest with the same command line. The SMC-related kernel
messages drop to single digits per 5 s (a brief burst during
AppleSMC init, then quiescence). `top` shows `kernel_task`
and `WindowServer` at nominal idle (~2 % / ~6 %).

A scriptable check:

```bash
# on the guest, captures 5 s of kernel log and greps for SMC errors
log stream --last 5s --predicate 'subsystem == "com.apple.kernel"' \
    2>/dev/null | grep -c 'SMC.*ERROR'
```

Expected: `>= 5000` before the fix, `<= 10` after.

## Unit-level sanity

The `#KEY` key can be read from the monitor-less debug path
once the device is instantiated:

```bash
# Open the QEMU monitor; the `ioport` command reads a single byte
# from the SMC command port.
(qemu) ioport readb 0x304   # status byte (0x00 when idle)
```

A direct userspace probe from the guest (`smc` tool from the
Unbound SMC Reader project or VirtualSMC's `smcread`) should
return a non-zero `#KEY` value equal to the number of keys
installed by `applesmc_isa_realize()` (with this series, 94;
without, the device returns 0x00 0x00 0x00 0x00).

## Regression check

Legacy macOS guests (10.11 - 10.13) which do not iterate the
key space via GET_KEY_BY_INDEX should boot unchanged. The
original six keys (REV/OSK0/OSK1/NATJ/MSSP/MSSD) are still
present and responded to with the same values.
