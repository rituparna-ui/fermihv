# FermiHV — Development Log

A from-scratch **Type-1 (bare-metal) AArch64 hypervisor**, built in a single
extended session, that boots real guest operating systems on QEMU's `virt`
machine with a Cortex-A72 (ARMv8.0-A, **no VHE**).

This document records *everything* we built, in order, and **why** each step
exists — the reasoning, the constraints, and the gotchas we hit and fixed.

---

## 1. The goal and the constraints

**Goal.** Start from nothing and end with a hypervisor that can boot an
unmodified **Linux** kernel as a guest — and, along the way, build every
mechanism a real Type-1 hypervisor needs (exception handling, stage-2
translation, vCPU world-switch, device emulation, a virtual interrupt
controller, SMP, and multi-tenancy).

**Target machine.** QEMU `virt`, `-cpu cortex-a72`, GICv3, virtualization
extensions on. We run **at EL2**; guests run at **EL1/EL0**.

**The single most important constraint: no VHE.** Cortex-A72 is ARMv8.0-A, so
the Virtualization Host Extensions (E2H) do **not** exist. VHE is exactly the
feature that would let `_EL1` registers transparently redirect to EL2 and let a
host kernel run at EL2 unmodified. Without it, FermiHV is a **non-VHE EL2
hypervisor**: it resides at EL2 and multiplexes EL1 guest worlds via stage-2
translation (`VTTBR_EL2`) and an explicit world switch. This single fact shaped
the entire design.

---

## 2. Environment & build pipeline (and why it's unusual)

- The dev host (an Amazon Cloud Desktop) has **no cross toolchain**, but a
  Docker container named `osdev` does: `aarch64-linux-gnu-gcc 13.3` +
  `qemu-system-aarch64 8.2.2` (modern; full `cortex-a72 + virtualization=on +
  gicv3` support).
- The project lives in `~/src/clawscratch/fermihv/` (a scratch root), kept
  **separate** from the user's existing `fermi-os` kernel project.
- **Why a snapshot image:** the `osdev` container only mounts the old `fermi-os`
  tree, so instead of disturbing it we `docker commit` it once into
  `fermihv-build:latest` and run **throwaway containers** that mount this
  project. `run.sh` orchestrates all of this. Tools (`dtc`, `wget`, `cpio`,
  `kmod`, `e2fsprogs`) were added to that image as milestones needed them.
- `run.sh` subcommands: `build`, `test` (boot the M0–M16 demos and dump
  serial), `linux` / `linux-raw` (boot Linux), `fermios` / `fermios-vgic`
  (boot fermi-os), `fetch-linux` / `fetch-busybox` / `fetch-modules` (pull the
  Linux kernel, busybox, and virtio modules), plus disk/initramfs/DTB build steps.

---

## 3. Architecture overview

```
EL2  FermiHV  (linked high at 0x70000000 so guests can own low RAM)
 │   vectors.S        16-entry VBAR_EL2; EL2-self faults -> C dispatcher,
 │                    guest traps -> __guest_exit
 │   world.S         __guest_enter / __guest_exit (KVM-style world switch)
 │   vcpu.c          struct vcpu, scheduler, demos, guest loaders
 │   stage2.c        stage-2 page tables (Normal / Device / per-VM)
 │   vgic.c          software GICv3 distributor/redistributor emulation
 │   gic.c / timer.c real GICv3 bring-up + EL2 CNTHP timer
 │   vdev.c          emulated PL011 UART + generic MMIO decode
 ▼
EL1  guest kernel (Linux / fermi-os) — its own MMU under our stage-2
 ▼
EL0  guest userspace (busybox shell / fermi-os shell)
```

Key invariant: **guest traps and EL2-self exceptions take different paths.**
The lower-EL vectors divert to `__guest_exit` (save guest, return to the C
scheduler); current-EL vectors go to `el2_common` → C dispatcher. The current
vCPU pointer rides in `TPIDR_EL2`.

---

## 4. The milestones — what and why

Each milestone ends in a **bootable, self-verifying** state and is its own git
commit. Every one boots in QEMU and prints evidence it worked.

### M0 — EL2 boot skeleton + PL011 UART
Boot at EL2 (via `virtualization=on`), bring up the serial console, confirm
`CurrentEL == 2`, park. **Why:** establish the whole toolchain/build/run loop
and prove we're actually at EL2 before writing any hypervisor logic.

### M1 — EL2 vector table + ESR decode + trap/recover
Install `VBAR_EL2`, a shared save/restore path, and a C dispatcher that decodes
`ESR_EL2` (EC/ISS), `ELR_EL2`, `FAR_EL2`. Prove it by issuing a deliberate
`BRK`, decoding it, advancing `ELR_EL2`, and resuming. **Why:** without a
working trap path, the very first guest trap would just hang the machine.

### M2 — First world switch (EL1 guest + HVC)
Set `HCR_EL2.RW=1` and `eret` into a bare EL1 guest blob; trap its `HVC` as a
lower-EL sync exception (EC=0x16). **Why:** this is the core hypervisor act —
dropping to a guest and getting control back via a trap.

### M3 — Stage-2 translation
Single-level (1 GiB block) identity stage-2 table; program `VTCR_EL2`
(39-bit IPA, `SL0=1`) + `VTTBR_EL2`; enable `HCR_EL2.VM`. Demonstrate an
on-demand stage-2 data abort (EC=0x24): read `HPFAR_EL2`, map the block,
**retry without advancing ELR**. **Why:** stage-2 is what isolates and remaps
guest memory — the foundation of VMs.
**Gotcha:** `HCR_EL2.DC=1` (Default Cacheable) so an MMU-off guest's accesses
are Normal memory under stage-2 (otherwise they're Device and a real kernel
can't run); but `DC=1` *forces the guest's stage-1 MMU to appear disabled*, so
guests that run their own MMU (Linux, fermi-os) must use **DC=0**.

### M4 — vCPU context + world-switch save/restore + scheduler
`struct vcpu` (GP regs in asm, EL1 sysreg bank in C, offsets guarded by
`_Static_assert`), KVM-style `__guest_enter`/`__guest_exit`, and a round-robin
scheduler. Two vCPUs each keep an independent counter across exits/re-entries.
**Why:** preserving full guest state across switches is what lets multiple
vCPUs/VMs coexist. The EL1 sysreg save/restore is the bulk of "context."

### M5 — MMIO trap-and-emulate (virtual UART)
Leave a UART region unmapped in stage-2; decode the data-abort `ISS`
(ISV/SAS/SRT/WnR) and emulate the access (forward bytes to the real console).
**Why:** trap-and-emulate is how a hypervisor gives guests *virtual* devices.

### M6 — Timer interrupts (a + b)
- **M6a:** GICv3 bring-up for EL2 + the EL2 physical timer (`CNTHP`, PPI 26).
  **Gotcha (the key fix):** physical IRQs only reach EL2 if `HCR_EL2.IMO=1`;
  with IMO=0 they target EL1 and, since we run at EL2 (a higher EL), they stay
  pending forever. Diagnosed via a register probe showing the IRQ pending but
  never taken.
- **M6b:** inject a *virtual* interrupt into a guest via the GICv3 list register
  `ICH_LR0_EL2`; the guest acks it on its virtual CPU interface (ICV).
**Why:** interrupts are how guests get timers and devices; injection via the
list registers is the GICv3 virtualization primitive.

### M7 — Boot a real Linux kernel  ⭐ (the original goal)
- Relocated the hypervisor to `0x70000000` so the guest owns low RAM.
- **Device passthrough:** identity-map the device block (GIC/UART/ITS) as
  stage-2 Device memory so Linux drives the *real* QEMU hardware; run with
  `HCR_EL2 = RW|VM|TSC` — **no DC** (Linux owns its MMU), **no IMO** (Linux
  takes its own physical IRQs at EL1).
- **PSCI stub** over the trapped `SMC` conduit (Linux uses SMC for PSCI).
- **Custom DTB** (`guest/guest.dts`): 768 MiB memory node (keeps the HV's RAM
  out of Linux's reach) + `earlycon` bootargs; embedded and memcpy'd to `x0`.
- **Result:** unmodified **Debian Linux 6.12** boots to `kernel_init` (PID 1).
**Gotchas:** QEMU auto-places a DTB at the RAM base for `-kernel`, so we embed
our own and place the kernel at `0x41000000`; and we use QEMU's generic loader
for the big Image.

### M8 — Linux to userspace (initramfs)
A tiny static, libc-free PID 1 (`guest/init.c`) packed (with a `/dev/console`
node) into a `newc` cpio.gz, located via `linux,initrd-start/end` injected into
the DTB. Linux unpacks it and runs `/init`, which heartbeats via `nanosleep`.
**Why:** proves the timer/scheduler work all the way through userspace.

### M9 — Interactive busybox shell
Static arm64 busybox initramfs; `/init` mounts proc/sys/devtmpfs, installs
applets, and `exec`s an interactive `sh`. **Why:** a real Linux prompt on the
hypervisor; `uname`, `ls`, `/proc/interrupts` (showing `arch_timer` firing via
GICv3) all run.

### M10 — Guest networking (virtio-net)
- **Why the earlier hang:** the virtio-net 64-bit BAR sat at `0x8000000000`,
  outside our 39-bit stage-2 IPA. Fix: QEMU `highmem=off` forces ECAM
  (`0x3f000000`) and all BARs into the low (<4 GB, block-0) region we already
  passthrough-map.
- `virtio_net` is a module in the stock kernel, so we stage its exact dep
  closure (`failover`, `net_failover`, `virtio_net`) for `6.12.86+deb13-arm64`,
  bundle them in the initramfs, and `insmod` them. `/init` brings up `eth0`
  (10.0.2.15 via SLIRP), resolves DNS, and **fetches a live web page over HTTP**.

### M11 — Persistent storage (virtio-blk + ext4)
Attach a virtio-blk disk (raw ext4 image pre-populated via `mke2fs -d`); the
guest mounts it, reads seeded data, and appends to a log that **survives
reboots** (boot 2 shows boot 1's entry). The fetch-modules step was generalized
to also stage `virtio_blk` + the `ext4` module stack.

### M12 — fermi-os as a guest (EL2→EL1→EL0)
The user's *own* from-scratch kernel boots to its interactive EL0 shell as a
guest — full three-level nesting. **Mechanism:** embed the fermi-os binary and
memcpy it to its native `0x40000000` at runtime (avoids a ROM-load overlap with
QEMU's auto-DTB). A `fermios-src/` scratch copy is tweaked (RAM capped so its
allocator can't reach the HV; virtio/FAT/net init skipped; only `task_shell`).
**Gotcha:** relinking fermi-os broke its hardcoded `0xFFFF000040000000`
assumptions — so we keep it at its native address and isolate via the embedded
copy instead.

### M13 — Software vGIC (the linchpin)
Stop passing the GIC through; **emulate** the GICv3 distributor/redistributor.
Leave GICD/GICR MMIO unmapped in stage-2 so guest accesses trap, and a software
`vgic.c` tracks per-INTID enable state. The hypervisor injects a virtual timer
via the list register **only when the guest enabled that INTID** in the
emulated GIC. **Why:** this is what unlocks SMP (virtual IPIs) and multi-tenancy
(no guest needs the real hardware).

### M14 — Virtual IPIs between vCPUs
Set `ICH_HCR_EL2.TC` (bit 8) to trap the guest's `ICC_SGI1R_EL1` writes; decode
the syndrome, route the SGI to the target vCPU's pending state, and inject it
via the list register when that vCPU runs. vCPU0 sends an SGI; vCPU1 receives
it on its virtual CPU interface. **Why:** IPIs are the heart of SMP.
**Gotcha:** `ICH_HCR_EL2` bit 10 is `TALL1` (traps *all* Group-1 ICC accesses —
it caught the PMR write in the first attempt); **TC is bit 8**.

### M15 — Preemptive dual-core guest
Tie it together: the EL2 timer preempts whichever vCPU runs; on each tick the
scheduler round-robins to the other. vCPU0 (producer) IPIs vCPU1 (consumer);
both make tens of millions of work units of concurrent progress. **Why:**
turns the IPI primitive into a working dual-core SMP guest.

### M16 — Multi-tenancy (two isolated VMs)
Each VM gets its **own stage-2 table + VMID** mapping the *same* guest IPA
(`0x40000000`) to a *different* host RAM block (VM0→`0x80000000`,
VM1→`0xC0000000`). Both run an identical guest yet are physically isolated; the
hypervisor's direct read of the two blocks shows distinct values. VMID-tagged
TLB entries make `VTTBR` switches flush-free. **Gotcha:** the machine caps RAM
below 4 GB, so we use `-m 3G` to get two 1 GiB-aligned blocks above the HV.

### M17 — A real kernel on the emulated vGIC (the long pole)
fermi-os boots to its shell with a ticking scheduler entirely on
**software-emulated** interrupt hardware — no passthrough. GIC and UART are
emulated; the **physical timer is trapped** (`CNTHCTL_EL2` traps `CNTP_*`,
emulated as no-ops) so it never fires a real interrupt; instead the EL2 `CNTHP`
drives ticks that we **inject as vINTID 30 through the vGIC**, gated on the
guest having enabled it. `CNTPCT` reads pass through for uptime. **Why:** this
is the bridge from "single passthrough guest" to "any number of isolated
tenants on virtual interrupts."

### M18 — *(in progress)* Per-VM vGIC + a real OS as a concurrent tenant
The capstone: combine M16 (per-VM isolation) + M17 (real OS on the vGIC) +
M15 (preemptive scheduling). The infrastructure change is making the vGIC state
**per-VM** (it was global) so two real guests don't clobber each other's
interrupt configuration. *(At the time of writing, the per-VM vGIC refactor in
`vgic.c` is done; the demo wiring and caller updates are pending — the tree is
mid-refactor and the M0–M17 work is the last committed, building state.)*

---

## 5. Cross-cutting technical decisions (the "why" behind the mechanisms)

- **Non-VHE EL2 design.** Everything is EL2-resident with explicit EL1 world
  switching, because cortex-a72 has no VHE.
- **`HCR_EL2` per use case.** `RW` (AArch64 EL1) is always set. `VM` enables
  stage-2. `DC` is set only for MMU-off toy guests (and must be *cleared* for
  real kernels that run their own MMU). `IMO` routes physical IRQs to EL2 (for
  injection / vGIC); it's *cleared* for device-passthrough guests so they take
  their own IRQs. `TSC` traps `SMC` (PSCI). `ICH_HCR_EL2.TC` traps SGIs (IPIs).
- **ELR semantics differ by trap type** (a recurring footgun): `HVC` and the
  preferred return for `SMC`-via-TSC point *past* the instruction or are
  advanced by 4; aborts that we want to *retry* must **not** advance ELR; an
  emulated MMIO/sysreg access advances by 4.
- **Stage-2 + VMID is the isolation mechanism.** Per-VM tables + distinct VMIDs
  give both memory isolation and flush-free TLB switching.
- **Device strategy is per-guest.** Linux uses **passthrough** (drives real
  QEMU hardware) for breadth; fermi-os has both a passthrough path and a fully
  **emulated-vGIC** path. Emulation is the route to multi-tenancy.
- **Guest images are external/embedded, not committed.** The Linux Image,
  busybox, and kernel modules are fetched on demand; fermi-os is a gitignored
  scratch copy embedded via `.incbin` with a placeholder fallback so the
  hypervisor always builds standalone.

---

## 6. Source map

```
src/boot.S          EL2 entry: stack, zero BSS, jump to hv_main
src/main.c          banner, EL check, runs the demo sequence, guest selector
src/vectors.S       VBAR_EL2 table + el2_common save/restore
src/world.S         __guest_enter / __guest_exit world switch
src/exception.{c,h} ESR decode, EC names, EL2-self + IRQ dispatch
src/vcpu.{c,h}      struct vcpu, scheduler, all demos, Linux/fermi-os loaders
src/stage2.{c,h}    stage-2 tables: Normal / Device / per-VM builders
src/vgic.{c,h}      software GICv3 distributor + virtual IPIs (per-VM)
src/gic.{c,h}       real GICv3 bring-up for EL2 (ack/eoi/enable)
src/timer.{c,h}     EL2 CNTHP timer (tick source for injection)
src/vdev.{c,h}      emulated PL011 UART + generic single-reg MMIO decode
src/mmio.h          MMIO accessors
src/uart.{c,h}      hypervisor's own PL011 console
src/guest.S         all linked-in guest blobs (counter, vuart, virq, vgic,
                    ipi, smp, tenant)
guest/              standalone guests: nano (own MMU), init.c/init.sh
                    (Linux userspace), guest.dts (Linux DTB)
linker.ld           HV at 0x70000000; flat identity, MMU off
Makefile            cross build + guest image/initramfs/dtb/disk rules
run.sh              container build/run orchestration + all run modes
```

## 7. How to run

```sh
./run.sh build                                   # compile
./run.sh test                                    # M0..M16 demos (serial dump)
./run.sh fetch-linux && fetch-busybox && fetch-modules
./run.sh linux                                   # Debian Linux -> networked busybox shell
# (fermi-os guest needs a fermios-src/ copy; see README)
./run.sh fermios                                 # fermi-os guest (passthrough)
./run.sh fermios-vgic                            # fermi-os on the emulated vGIC
```

## 8. Status

**M0–M17 are committed and verified** (18 commits). FermiHV boots two real OSes
(Linux to a networked, persistent-storage busybox shell; fermi-os to its EL0
shell, both via passthrough and on the fully emulated vGIC), does SMP with
virtual IPIs, and runs memory-isolated concurrent VMs. **M18** (per-VM vGIC +
a real OS as a concurrent isolated tenant) is in progress.
