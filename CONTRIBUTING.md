# Contributing to mos-qemu

`mos-qemu` is a fork of [qemu-project/qemu](https://gitlab.com/qemu-project/qemu)
carrying specific patches for running macOS 15 (Sequoia) as a guest.
Every code change here must be authored as if it were going up to
qemu-devel tomorrow — see the [`upstream-pr/`](upstream-pr/) staging
area for the four series already prepared for upstream submission.

Before contributing code, please read [`README.md`](README.md) for an
overview of the patch surface, and skim the current state of
[`upstream-pr/README.md`](upstream-pr/README.md) so you know what's
already in flight.

## Reporting bugs

File issues on GitHub: https://github.com/MattJackson/mos-qemu/issues

Please include:

- Affected version (`git rev-parse HEAD` of this repo, plus upstream
  QEMU version — usually 11.0.0).
- Whether you're hitting the bug via [mos-docker](https://github.com/MattJackson/mos-docker)
  (the integrated runtime) or in a standalone build.
- Reproduction steps. Serial-log excerpts (`data/logs/serial-*.log`
  if running under mos-docker) are extremely helpful.

For bugs that are clearly in unmodified upstream QEMU, please report
them to the QEMU project directly via
https://www.qemu.org/contribute/report-a-bug/. If you are unsure
whether a bug is in our patches or in upstream code, file here and
we will help triage.

For **security issues**, do not use the public tracker — see
[`SECURITY.md`](SECURITY.md).

## Upstream-submission discipline

Every code change in this repo must be PR-ready against upstream
QEMU. That means:

- **Commit message format:** `hw/<subsystem>/<file>: <subject>` —
  subsystem prefix, imperative mood, no trailing period, ~75-char
  wrap. Match recent upstream commits for cadence.
- **Licensing:** new files are GPL-2.0-or-later (matching upstream
  QEMU's prevailing license). QEMU-derived files retain their
  upstream license headers verbatim.
- **Coding style:** 4-space indent, K&R braces on functions; match
  the existing conventions in `hw/display/` and `hw/misc/`.
- **Logging:** use `qemu_log_mask(LOG_GUEST_ERROR, ...)`,
  `trace_*`, or `warn_report` for runtime diagnostics. No inline
  `printf` or bare `qemu_log`.
- **No fork-local identifiers in C/H source or patch-added lines:**
  never reference `mos15`, `mos-qemu`, `mos-docker`, `MattJackson`,
  `mjackson`, or `pq.io` in source, debug strings, or comments.
  Generic identifiers only. CI's `forbidden-identifiers` job will
  reject the commit otherwise.
- **No personal paths or credentials.** Grep for usernames, internal
  IPs, tokens before committing.
- **Trailers:** committed work may carry `Signed-off-by:` trailers;
  upstream submissions require them. Do **not** include
  `Co-Authored-By: Claude` (or any AI/agent attribution) on commits
  destined for upstream.

## Adding a new patch series

Stage new upstream-destined work under `upstream-pr/<topic>/`
alongside the existing four series:

- `applesmc-fix/` — `GET_KEY_BY_INDEX` correction + key population
- `apple-gfx-pci-linux/` — Linux C port of `apple-gfx-pci.m`
- `usb-hid-apple-ids/` — `apple-kbd` / `apple-mouse` / `apple-tablet`
  wrapper devices
- `vmware-svga-caps/` — VMware SVGA II capability bits + 5K cap

Each package carries:

- `SERIES.md` — cover letter for the patch series
- `0001-*.patch`, `0002-*.patch`, ... — `git format-patch` output
- `PR_DESCRIPTION.md` — GitHub-style description
- `TESTING.md` — validation recipe

CI (`upstream-pr-apply` job) runs `git am --check` on every series
against an unmodified upstream QEMU 11.0.0 tarball; a series that no
longer applies cleanly fails the gate.

## Testing

CI runs against the QEMU version pinned in
[`.github/workflows/ci.yml`](.github/workflows/ci.yml) (env
`QEMU_VERSION`). Three jobs gate every change:

1. `clang-syntax` — `clang -fsyntax-only` on every overlay file
   against upstream QEMU's headers (cheap drift check).
2. `meson-setup` — full `./configure` + time-boxed `make` of an
   overlay-applied upstream tree (catches Kconfig / meson / trace
   drift).
3. `upstream-pr-apply` — every `upstream-pr/*/` series applies cleanly.
4. `forbidden-syntax-patterns` and `forbidden-identifiers` —
   static scans for QEMU-10-removed APIs and fork-local identifiers.

For local end-to-end validation, build a binary (see "Standalone
build" in `README.md`) and run a regression phase via the
[mos-docker](https://github.com/MattJackson/mos-docker) test image:
`./mos test 0` through `./mos test 4`. The `launch-phase-N.sh`
scripts at this repo's root are the SSH-the-server shortcut form.

## License

This repository carries [AGPL-3.0](LICENSE) on its additions.
Upstream-derived files retain GPL-2.0-or-later as required by QEMU's
licensing. The combined work satisfies both via the AGPL-3.0 →
GPL-3.0 → GPL-2.0-or-later compatibility chain.
