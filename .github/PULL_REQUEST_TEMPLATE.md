## What this PR does

<!-- 1-2 sentences describing the change. -->

## Why

<!-- Context / motivation. Link any mos / mos15 tracker issue that
     motivated the change. -->

## Test plan

<!-- Reproducible steps a reviewer can run locally. Prefer the smallest
     invocation that exercises the change (qtest, check-qtest target,
     or a concrete `qemu-system-*` command line). -->

```
make check
```

## Verification

- [ ] I ran the relevant portion of the QEMU test suite locally and it passed.
- [ ] `scripts/checkpatch.pl` is clean (or deviations are noted below).
- [ ] Commit messages follow QEMU's `subsystem: summary` convention.

## Upstream submission readiness

- [ ] This change is ready to be mailed to `qemu-devel@nongnu.org`.
- [ ] This change is staging-only (needs more work before upstream).
- [ ] This change is mos-specific and will not be upstreamed.

> Reminder: upstream QEMU accepts patches through the `qemu-devel`
> mailing list, not GitHub PRs. This repository is a staging ground;
> landing here is a prerequisite step, not the submission itself.

## Linked issues

<!-- Fixes #..., Refs #... -->
