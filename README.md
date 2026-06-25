# FermiHV

A Type-1 (bare-metal) AArch64 hypervisor written from scratch for QEMU's
`virt` machine with a Cortex-A72 (ARMv8.0-A, **no VHE**). FermiHV runs at EL2
and hosts guests at EL1 — culminating in booting an unmodified **Linux
kernel** as a guest.

## Status

All milestones boot and self-verify. The headline result: a real Debian
**Linux 6.12** kernel boots as a guest **to userspace** — its PID 1 `/init`
(from an initramfs) prints on the console and heartbeats via `nanosleep`,
exercising the guest's MMU, the ARM generic timer, GICv3 interrupts, PCIe
enumeration, and PSCI, all hosted by the from-scratch EL2 hypervisor.

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
| M8 | **Linux reaches userspace** (PID 1 `/init` via an initramfs) |
| M9 | **Interactive busybox shell** on the Linux guest |
| M10 | **Guest networking**: virtio-net + DNS + HTTP over SLIRP NAT |
| M11 | **Persistent storage**: virtio-blk + ext4, writes survive reboots |
| M12 | **fermi-os as a guest**: another from-scratch kernel boots to its EL0 shell (EL2→EL1→EL0) |
| M13 | **Software vGIC**: emulated GICD/GICR (trap+emulate) gates virtual interrupt injection |
| M14 | **Virtual IPIs**: vCPU0 sends an SGI to vCPU1 through the vGIC (SMP groundwork) |
| M15 | **Preemptive dual-core guest**: two time-sliced vCPUs coordinating via IPIs |
| M16 | **Multi-tenancy**: two isolated VMs (per-VM stage-2 + VMID), same IPA, distinct host RAM |
| M17 | **Real kernel on the vGIC**: fermi-os boots on the emulated GIC (no passthrough) |
| M18 | **Per-VM vGIC**: two isolated VMs, each on its own emulated GIC, concurrent + time-sliced |

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
./run.sh fetch-busybox  # download a static arm64 busybox (once, for a shell)
./run.sh fetch-modules  # stage matching virtio_net kernel modules (once, for networking)
./run.sh linux          # boot Linux + interactive busybox shell (Ctrl-A X to quit)
./run.sh linux-raw      # same, captured non-interactively (8s+ serial dump)
```

`build/Image` (Linux kernel), `build/busybox`, and `build/netmods` are fetched
separately and not committed. With busybox present the initramfs gives an
interactive shell; with the modules present the guest brings up a virtio-net
NIC (QEMU usermode/SLIRP) — `/init` configures `eth0` (10.0.2.15), resolves
DNS, and fetches a page over HTTP to prove connectivity.

### Booting fermi-os as a guest (M12)

FermiHV can also host another from-scratch kernel, [fermi-os](https://github.com/rituparna-ui/fermi-os),
which boots to its own EL0 shell (so the stack is EL2 hypervisor → EL1 guest
kernel → EL0 guest userspace). Put a copy at `fermios-src/` (gitignored) with
three guest-friendly tweaks, then run `./run.sh fermios`:

- `src/mm/pmm/pmm.h`: cap `MEM_SIZE` (e.g. 256 MiB) so its allocator can't
  reach the hypervisor's high RAM.
- `src/kernel.c`: skip the PCI/virtio/FAT/net init (high-MMIO devices aren't
  passed through) and keep only `task_shell`.

The hypervisor embeds the resulting `kernel.bin` and memcpy's it to its native
`0x40000000` at runtime (the embedded approach avoids a ROM-load overlap with
QEMU's auto-DTB at the RAM base). The build falls back to a placeholder when
`fermios-src/` is absent, so the hypervisor always builds standalone.
