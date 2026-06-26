# FermiHV — Project Overview

A from-scratch **Type-1 (bare-metal) AArch64 hypervisor**, written in C and
assembly, that runs at EL2 on QEMU's `virt` machine and boots real, unmodified
guest operating systems. It was built incrementally across 31 milestones
(M0–M30), each a self-contained, demonstrable step from "prints a character at
EL2" to "two real OSes running concurrently as isolated tenants, plus emulated
virtio devices and the SMP virtualization plumbing."

---

## 1. Target & toolchain

| | |
|---|---|
| Machine | QEMU `virt`, `-cpu cortex-a72`, GICv3, **no VHE** (true EL2/EL1 split) |
| ISA | AArch64 (ARMv8-A) |
| Entry | QEMU `virtualization=on` drops `-kernel` at **EL2**, MMU/caches off |
| HV load address | `0x70000000` (high RAM; guests own low RAM from `0x40000000`) |
| Memory | `-m 3G` (RAM `0x40000000`–`0x100000000`) |
| Build/run | Dockerised aarch64 cross-toolchain via `./run.sh build` / `./run.sh test` |
| Size | ~4,900 lines across `src/` |

The hypervisor is a single statically-linked image. There is no host OS — FermiHV
*is* the most-privileged software on the machine.

---

## 2. Architecture at a glance

```
        QEMU virt  (EL2 entry, MMU off)
              │
        boot.S  ── stack, BSS, enable FP/SIMD (CPTR_EL2.TFP=0),
              │     enable GIC sysreg interface (ICC_SRE_EL2)
        hv_main()  ── runs the milestone demos, then a guest
              │
   ┌──────────┴───────────────────────────────────────────┐
   │  EL2 hypervisor                                        │
   │   • vectors.S / exception.c  — EL2 trap dispatch       │
   │   • stage2.c                 — stage-2 MMU (per-VM)    │
   │   • world.S + vcpu.c         — world switch + scheduler│
   │   • vgic.c                   — software GICv3          │
   │   • vvirtio.c                — virtio-console + blk     │
   │   • gic.c / timer.c          — real GICv3 + EL2 timer  │
   └──────────┬───────────────────────────────────────────┘
              │  eret / trap (HVC, stage-2 abort, IRQ, sysreg)
        ┌─────┴─────┐        ┌───────────┐
        │ EL1 guest │  ...   │ EL1 guest │   (each in its own stage-2 + VMID)
        │  (Linux)  │        │ (fermi-os)│
        └───────────┘        └───────────┘
```

**Control flow per vCPU:** `vcpu_run_once()` restores the vCPU's EL1 system
registers, FP/SIMD, and virtual-GIC context, then `__guest_enter` (`world.S`)
`eret`s into the guest. The guest runs until it traps back through the EL2
vector table (`vectors.S` → `__guest_exit`), which saves guest state and returns
to C. `exception.c` decodes `ESR_EL2` and dispatches: HVC, stage-2 abort
(fault-in RAM or emulate device MMIO), IRQ (timer/GIC), or sysreg trap.

---

## 3. Subsystems

- **Boot (`boot.S`)** — parks secondaries, sets the EL2 stack, zeroes BSS,
  enables FP/SIMD and the GIC system-register interface, jumps to C.
- **Exceptions (`vectors.S`, `exception.c`)** — full EL2 vector table; ESR
  decode; stage-2 abort handling; IRQ handling; CNTP and PSCI emulation.
- **Stage-2 MMU (`stage2.c`)** — per-VM identity/offset translation tables,
  `VTCR`/`VTTBR` programming, on-demand 1 GB-block fault-in, device mappings,
  per-VM builder with VMID for isolation.
- **vCPU + world switch (`vcpu.c`, `world.S`, `vcpu_offsets.h`)** — full GP +
  EL1 sysreg + FP/SIMD + **per-vCPU virtual-GIC** context save/restore; a
  round-robin / time-sliced scheduler.
- **Software vGIC (`vgic.c`)** — emulated GICv3 distributor + per-vCPU
  redistributor frames (trap-and-emulate of GICD/GICR MMIO), virtual interrupt
  injection via `ICH_LR0`, virtual SGIs (IPIs), and the PIDR2/TYPER identity
  registers Linux's `gic-v3` driver probes.
- **Real GIC + timer (`gic.c`, `timer.c`)** — EL2-side GICv3 bring-up and the
  EL2 physical timer used as the preemption tick.
- **Virtual devices (`vdev.c`, `vvirtio.c`)** — emulated PL011 UART; legacy
  virtio-mmio **console** and **block** devices, each per-VM, with split
  virtqueue processing, used-ring completion, and completion IRQs via the vGIC.
- **Guests (`guest.S`, `guest_*.S`, `guest/`)** — many small position-independent
  test guests embedded in the image, plus `incbin`-embedded real kernels
  (Linux `Image`, `fermi-os`) and a compiled DTB.

---

## 4. Milestones (M0–M30)

| # | What it proves |
|---|---|
| M0 | Boots at EL2, PL011 console |
| M1 | EL2 vector table, `ESR_EL2` decode, trap + recover |
| M2 | First world switch: `eret` to an EL1 guest, trap its `HVC` |
| M3 | Stage-2 translation (`VTCR`/`VTTBR`) + on-demand fault-in |
| M4 | vCPU context + full save/restore + round-robin scheduler |
| M5 | MMIO trap-and-emulate (virtual UART) |
| M6a | GICv3 bring-up + EL2 physical timer IRQ (`CNTHP`) |
| M6b | Virtual interrupt injection (`ICH_LR0_EL2`) → guest virtual timer |
| M7 | Load an external guest image; guest runs its own stage-1 MMU |
| M7 (goal) | **Boot a real Linux kernel** (device passthrough + DTB + PSCI) |
| M8 | **Linux reaches userspace** (PID 1 `/init` via initramfs) |
| M9 | **Interactive busybox shell** on the Linux guest |
| M10 | **Guest networking**: virtio-net + DNS + HTTP over SLIRP NAT |
| M11 | **Persistent storage**: virtio-blk + ext4, writes survive reboots |
| M12 | **fermi-os as a guest**: another from-scratch kernel boots to its EL0 shell |
| M13 | **Software vGIC**: emulated GICD/GICR gates virtual interrupt injection |
| M14 | **Virtual IPIs**: vCPU0 → SGI → vCPU1 through the vGIC |
| M15 | **Preemptive dual-core guest**: two time-sliced vCPUs coordinating via IPIs |
| M16 | **Multi-tenancy**: two isolated VMs (per-VM stage-2 + VMID) |
| M17 | **Real kernel on the vGIC**: fermi-os on the emulated GIC (no passthrough) |
| M18 | **Per-VM vGIC**: two isolated VMs, each on its own emulated GIC |
| M19 | **Real OS as a tenant**: fermi-os to its shell concurrently with another VM |
| M20 | **Linux on the emulated vGIC** (kernel + virtual timer, no passthrough) |
| M21 | **Two real OSes, co-resident**: Linux + fermi-os concurrent, isolated |
| M22 | **Guest-driven SMP**: a guest boots its secondary core via PSCI `CPU_ON` + IPI |
| M23 | **Emulated virtio device**: guest prints via a virtio-console (virtqueue) |
| M24 | **Interrupt-driven virtio**: used ring + completion IRQ via the vGIC |
| M25 | **virtio-blk**: guest writes a sector via a request chain to a backing disk |
| M26 | **virtio-blk read** verified: guest reads back a hypervisor-seeded sector |
| M27 | **Per-VM virtio devices**: two tenants each drive their own virtio-console |
| M28 | **Per-VM virtio-blk**: each tenant gets its own isolated backing disk |
| M29 | **Per-vCPU GIC context**: two vCPUs, each with independent virtual-interrupt state |
| M30 | **Per-vCPU GICR frames**: each vCPU owns a redistributor at a distinct address |

Plus a committed correctness fix: **FP/SIMD save/restore across the world
switch** (guests that use floating point/NEON, like Linux, need their `q`
registers preserved on every switch).

---

## 5. Source map (`src/`)

| File | Role |
|---|---|
| `boot.S` | EL2 boot stub (stack, BSS, FP, GIC SRE) |
| `vectors.S` | EL2 exception vector table + common save path |
| `world.S` | `__guest_enter` / `__guest_exit` world switch (GP + FP/SIMD) |
| `exception.c/.h` | EL2 trap dispatch: HVC, stage-2 abort, IRQ, sysreg, CNTP, PSCI |
| `stage2.c/.h` | Stage-2 MMU: identity + per-VM tables, VTCR/VTTBR, fault-in |
| `vcpu.c/.h`, `vcpu_offsets.h` | vCPU struct, sysreg/FP/vGIC save-restore, scheduler, **all milestone demos** |
| `vgic.c/.h` | Software GICv3 distributor + per-vCPU redistributor + SGIs |
| `gic.c/.h` | Real GICv3 bring-up at EL2 |
| `timer.c/.h` | EL2 physical timer (preemption tick) |
| `vdev.c/.h`, `mmio.h` | Virtual PL011 UART + generic MMIO decode |
| `vvirtio.c/.h` | Per-VM virtio-console + virtio-blk device models |
| `uart.c/.h` | Physical PL011 driver + formatted print |
| `guest.S` | Embedded position-independent test guests (one per demo) |
| `guest_image.S`, `guest_dtb.S`, `guest_fermios.S` | `incbin`-embedded Linux Image, DTB, fermi-os binary |
| `main.c` | `hv_main()` — runs the demo sequence, then selects a real guest |
| `linker.ld` | Flat image linked at `0x70000000` |

**`guest/`** holds standalone guest sources: `nano.c`/`nano_boot.S`/`nano.ld`
(a tiny kernel that brings up its own stage-1 MMU), `init.c`/`init.sh` (Linux
PID-1 init for the initramfs), and `guest.dts` (the guest device tree).

---

## 6. Build & run

```bash
./run.sh build              # cross-compile the HV image in Docker
./run.sh test               # boot the M0–M30 demo sequence
```

Real-guest run modes (each sets a marker word and boots a real OS):

| Mode | Boots |
|---|---|
| `./run.sh linux` / `linux-raw` | Linux (Debian 6.12 arm64) → networked busybox shell, **device passthrough** |
| `./run.sh linux-vgic-raw` | Linux on the **emulated vGIC** (kernel + virtual timer) |
| `./run.sh fermios` / `fermios-raw` | fermi-os → its EL0 shell, passthrough |
| `./run.sh fermios-vgic-raw` | fermi-os on the emulated vGIC |
| `./run.sh mtenant-os-raw` | Linux + fermi-os as **two concurrent isolated tenants** |
| `./run.sh mtenant-real-raw` | real OS + a lightweight tenant, concurrent + isolated |
| `fetch-linux` / `fetch-busybox` / `fetch-modules` | download guest artifacts |

Verification anchors used throughout development: Linux prints
`FERMIHV_SHELL_OK`; fermi-os prints `Welcome to the Fermi shell.`

---

## 7. Repository & branches

- **Remote:** `git@github.com:rituparna-ui/fermihv.git`
- **Branches:** a single **`master`** branch (local) tracking **`origin/master`**.
  There are **no feature branches** — the project is a clean, linear history
  where each commit is one milestone (or a focused fix), committed and pushed
  in order.
- **Commits:** 35, one per milestone plus the FP/SIMD fix and an early
  HV-relocation refactor. The commit subjects map 1:1 to the milestone table
  (e.g. `feat(m30): per-vCPU GICR frames (SMP redistributor layout)`).
- **Conventions:** Conventional Commits; each milestone is built and its demo
  verified (and the real OSes re-checked for regressions) before commit.

```
8b4afee feat(m30): per-vCPU GICR frames (SMP redistributor layout)
2cbfeed feat(m29): per-vCPU virtual GIC context (SMP foundation)
145bcd8 feat(m28): per-VM virtio-blk (each tenant gets its own disk)
094df45 feat(m27): per-VM virtio devices (each tenant gets its own console)
c9f49b3 feat(m26): virtio-blk read path (guest reads a seeded sector)
e1005a2 feat(m25): virtio-blk device (block read/write over a backing disk)
35ffdbb feat(m24): interrupt-driven virtio-console (used ring + completion IRQ)
5e8a0a9 feat(m23): emulated virtio-console device (virtqueue)
539afef fix: save/restore guest FP/SIMD across the world switch
863c324 feat(m22): guest-driven SMP via PSCI CPU_ON
 ...     ... (m21 … m1)
fbfe6e9 feat(m0): EL2 boot skeleton with PL011 UART
```

(See `DEVLOG.md` for the full narrative development log.)

---

## 8. What works today

- A real **Linux** kernel and a second from-scratch kernel (**fermi-os**) each
  boot to an interactive shell — under passthrough *and* on the fully software-
  emulated GICv3.
- **Two real OSes run concurrently** as isolated tenants (separate stage-2
  translation + VMID + per-VM vGIC + per-VM virtio devices).
- A from-scratch **virtio device layer**: console and block devices, read and
  write, interrupt-driven completion via the vGIC, with **per-VM isolation**.
- **SMP plumbing**: virtual IPIs, PSCI `CPU_ON`, per-vCPU virtual-GIC context
  (list register + CPU interface) preserved across preemption, and per-vCPU
  GICR frames — the building blocks of a multi-vCPU guest.

## 9. Known limitations / future work

- **linux-vgic** boots the kernel and virtual timer on the emulated vGIC but
  does not deliver device SPIs (e.g. the UART IRQ), so it stops at `/init`
  rather than an interactive console. Full SPI routing to a guest is future work.
- **Multi-vCPU Linux** is not yet booted end-to-end: the remaining pieces are a
  64-bit `GICR_TYPER` read path + a 2-CPU guest DTB (MPIDR/affinity matching),
  on top of the per-vCPU GICR frames and context now in place.
- virtio devices are minimal (single queue, legacy mmio); no virtio-net in the
  emulated path (networking today uses passthrough virtio-net).
