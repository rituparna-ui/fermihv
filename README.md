# FermiHV

A Type-1 (bare-metal) AArch64 hypervisor written from scratch for QEMU's
`virt` machine with a Cortex-A72 (ARMv8.0-A, **no VHE**). FermiHV runs at EL2
and hosts guests at EL1 — culminating in booting an unmodified **Linux
kernel** as a guest.

## Status

All milestones boot and self-verify. The headline result: a real Debian
**Linux 6.12** kernel boots as a guest to userspace (PID 1), exercising its
own MMU, the ARM generic timer, GICv3 interrupts, and PCIe enumeration, then
calls PSCI — which the hypervisor handles.

| Milestone | What it proves |
|-----------|----------------|
| M0 | Boots at EL2, PL011 console |
| M1 | EL2 vector table, `ESR_EL2` decode, trap + recover |
| M2 | First world switch: `eret` to an EL1 guest, trap its `HVC` |
| M3 | Stage-2 translation (`VTCR`/`VTTBR`) + on-demand fault-in |
| M4 | `vcpu` context + full save/restore + round-robin scheduler |
| M5 | MMIO trap-and-emulate (virtual UART) |
| M6a | GICv3 bring-up + EL2 physical timer IRQ (`CNTHP`) |
| M6b | Virtual interrupt injection (`ICH_LR0_EL2`) → guest virtual timer |
| M7 | Load an external guest image; guest runs its own stage-1 MMU |
| M7 (goal) | **Boot a real Linux kernel** (device passthrough + DTB + PSCI) |

## Architecture

- **EL2-resident, non-VHE.** Guests run at EL1/EL0; worlds are multiplexed via
  stage-2 (`VTTBR_EL2`) and a KVM-style world switch (`world.S`).
- **boot.S** EL2 entry; the hypervisor is linked high (`0x70000000`) so a guest
  can own the low RAM Linux expects.
- **vectors.S** 16-entry `VBAR_EL2`; guest traps divert to `__guest_exit`.
- **vcpu.c** vCPU state, scheduler, MMIO emulation, PSCI, and the Linux loader.
- **stage2.c** 1 GiB-block stage-2 (Normal + Device/passthrough).
- **gic.c / timer.c** GICv3 + `CNTHP_EL2` for the EL2 tick / vGIC injection.

### Linux boot specifics
- Hypervisor relocated to `0x70000000`; guest gets RAM `0x40000000..0x6FFFFFFF`
  (the embedded DTB's memory node keeps the HV region out of Linux's reach).
- Device MMIO (GIC/UART/ITS, block 0) is **passthrough** identity-mapped as
  Device memory; `HCR_EL2 = RW|VM|TSC` (no `DC`, no `IMO`) so Linux owns its MMU
  and drives the real GICv3/timer directly.
- A minimal **PSCI** stub handles the guest's `SMC` conduit.
- The DTB (`guest/guest.dts`) carries `earlycon` bootargs; QEMU's generic loader
  places the kernel `Image`, the hypervisor places the DTB.

## Build & run

Requires Docker; the build runs in a container snapshotted from `osdev`
(aarch64-linux-gnu gcc + qemu-system-aarch64).

```sh
./run.sh build          # compile
./run.sh test           # boot M0..M7 demos for 8s and print serial
./run.sh fetch-linux    # download a prebuilt arm64 Linux Image (once)
./run.sh linux          # boot FermiHV + the Linux guest
```

`build/Image` (the Linux kernel) is fetched separately and not committed.
