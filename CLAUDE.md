# qemu-mos15

QEMU fork carrying the host-side paravirtualized GPU device for the
**mos** project — running unmodified macOS 15 in QEMU/KVM with Metal
working through Vulkan via lavapipe.

This repo is one of several mos satellites. **Project context, current
state, mental model, build/deploy, and don't-do-this list all live in
the parent project's CLAUDE.md and library:**

- Airport map + working mode + current state: `../mos/CLAUDE.md`
- Onboarding for fresh sessions: `../mos/ONBOARDING-PROMPT.md`
- Standing rules + closed-milestone facts: `../mos/memory/MEMORY.md`
- Wire protocol / class layouts / state machines: `../mos/paravirt-re/library/`

## Scope of this repo

Host-side QEMU patches that implement the paravirt PCI device:

- `hw/display/apple-gfx-pci-linux.c` — PCI device wrapper
- `hw/display/apple-gfx-common-linux.c` — shared FIFO/MMIO/MSI plumbing

Anything Vulkan-side or protocol-decoder lives in
`/Users/mjackson/Developer/libapplegfx-vulkan/`. Anything macOS-side
is in `/Users/mjackson/Developer/mos-staging/` (data) and
`/Users/mjackson/Developer/mos15-patcher/`.

## Conventions

- **Upstream-clean commits.** All edits in this fork must be
  PR-ready: subsystem prefix in commit titles, no personal paths,
  no `Co-Authored-By: Claude` trailers. See
  `../mos/memory/feedback_upstream_submission.md`.
- For build/deploy commands, see `../mos/CLAUDE.md` "Build / deploy".
- For the working mode (UNATTENDED for M5 work), see
  `../mos/CLAUDE.md` "Working mode" and
  `../mos/memory/feedback_m5_unattended.md`.
