#!/usr/bin/env bash
#
# Host-side build/run wrapper for FermiHV.
#
# The aarch64 toolchain + qemu live inside the running `osdev` container, but
# that container only mounts the fermi-os tree. Rather than disturb it, we
# snapshot its filesystem into a reusable image once, then run throwaway
# containers that mount THIS project directory.
#
# Usage:
#   ./run.sh build        # compile (default)
#   ./run.sh run          # build + boot in QEMU (Ctrl-A X to quit)
#   ./run.sh test         # build + boot for 8s, capture serial, check for EL2
#   ./run.sh clean
#   ./run.sh shell        # interactive shell in the build container
set -euo pipefail

IMAGE="fermihv-build:latest"
SRC_CONTAINER="osdev"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ensure_image() {
	if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
		echo "[run.sh] snapshotting $SRC_CONTAINER -> $IMAGE (one-time)"
		docker commit "$SRC_CONTAINER" "$IMAGE" >/dev/null
	fi
}

drun() { docker run --rm -v "$HERE":/work -w /work "$IMAGE" "$@"; }

cmd="${1:-build}"
ensure_image
case "$cmd" in
	build|all) drun make all ;;
	clean)     drun make clean ;;
	run)       drun make run ;;
	debug)     drun make debug ;;
	shell)     docker run --rm -it -v "$HERE":/work -w /work "$IMAGE" bash ;;
	fetch-linux)
		echo "[run.sh] fetching a prebuilt arm64 Linux Image -> build/Image"
		drun bash -c 'mkdir -p build && wget -q -O build/Image \
		  http://deb.debian.org/debian/dists/stable/main/installer-arm64/current/images/netboot/debian-installer/arm64/linux \
		  && ls -la build/Image'
		;;
	fetch-busybox)
		echo "[run.sh] fetching static arm64 busybox -> build/busybox"
		drun bash -c 'set -e; base=http://deb.debian.org/debian/pool/main/b/busybox/; \
		  deb=$(wget -qO- "$base" | grep -oE "busybox-static_[^\"]*_arm64\.deb" | sort -u | tail -1); \
		  wget -q -O /tmp/bb.deb "$base$deb"; rm -rf /tmp/bbx; mkdir -p /tmp/bbx; \
		  dpkg-deb -x /tmp/bb.deb /tmp/bbx; mkdir -p build; \
		  cp "$(find /tmp/bbx -path "*bin/busybox" -type f | head -1)" build/busybox; \
		  chmod +x build/busybox; ls -la build/busybox'
		;;
	linux)
		drun make all
		echo "[run.sh] booting FermiHV + Linux guest (interactive; Ctrl-A X to quit)"
		# -it gives the guest's busybox shell a real terminal on the serial console.
		docker run --rm -it -v "$HERE":/work -w /work "$IMAGE" \
			qemu-system-aarch64 \
			-machine virt,gic-version=3,virtualization=on -cpu cortex-a72 \
			-m 2G -nographic -nic none -kernel build/fermihv.elf \
			-device loader,file=build/Image,addr=0x41000000,force-raw=on \
			-device loader,file=build/guest.dtb,addr=0x48000000,force-raw=on \
			-device loader,file=build/initramfs.cpio.gz,addr=0x4c000000,force-raw=on
		;;
	linux-demo)
		# Non-interactive: pipe a canned command sequence into the guest shell.
		drun make all
		printf 'uname -a\ncat /proc/cpuinfo | head -6\nls -la /\ncat /proc/interrupts\necho FERMIHV_SHELL_OK\npoweroff -f\n' | \
		docker run --rm -i -v "$HERE":/work -w /work "$IMAGE" bash -c 'timeout 95 qemu-system-aarch64 \
			-machine virt,gic-version=3,virtualization=on -cpu cortex-a72 \
			-m 2G -nographic -nic none -kernel build/fermihv.elf \
			-device loader,file=build/Image,addr=0x41000000,force-raw=on \
			-device loader,file=build/guest.dtb,addr=0x48000000,force-raw=on \
			-device loader,file=build/initramfs.cpio.gz,addr=0x4c000000,force-raw=on \
			2>&1 || true'
		;;
	linux-raw)
		drun make all
		echo "[run.sh] booting FermiHV + Linux guest (captured, ${LINUX_TIMEOUT:-95}s)..."
		drun bash -c 'timeout 95 qemu-system-aarch64 \
			-machine virt,gic-version=3,virtualization=on -cpu cortex-a72 \
			-m 2G -nographic -nic none -kernel build/fermihv.elf \
			-device loader,file=build/Image,addr=0x41000000,force-raw=on \
			-device loader,file=build/guest.dtb,addr=0x48000000,force-raw=on \
			-device loader,file=build/initramfs.cpio.gz,addr=0x4c000000,force-raw=on \
			2>&1 || true'
		;;
	test)
		drun make all
		echo "[run.sh] booting for 8s, capturing serial..."
		drun bash -c 'timeout 8 qemu-system-aarch64 \
			-machine virt,gic-version=3,virtualization=on -cpu cortex-a72 \
			-m 2G -nographic -nic none -kernel build/fermihv.elf 2>&1 || true'
		;;
	*) echo "unknown command: $cmd" >&2; exit 2 ;;
esac
