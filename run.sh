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
	linux)
		drun make all
		echo "[run.sh] booting FermiHV + Linux guest (image+dtb via loader)..."
		drun bash -c 'timeout 95 qemu-system-aarch64 \
			-machine virt,gic-version=3,virtualization=on -cpu cortex-a72 \
			-m 2G -nographic -nic none -kernel build/fermihv.elf \
			-device loader,file=build/Image,addr=0x41000000,force-raw=on \
			-device loader,file=build/guest.dtb,addr=0x48000000,force-raw=on \
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
