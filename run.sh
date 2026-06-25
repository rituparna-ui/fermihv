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
	fetch-modules)
		echo "[run.sh] staging guest kernel modules -> build/netmods"
		drun bash -c '
		  set -e
		  ver=6.12.86+deb13-arm64
		  wget -q -O /tmp/li.deb "http://deb.debian.org/debian/pool/main/l/linux/linux-image-${ver}-unsigned_6.12.86-1_arm64.deb"
		  rm -rf build/_li && mkdir -p build/_li && dpkg-deb -x /tmp/li.deb build/_li
		  mkdir -p build/_li/lib && ln -sf "$(pwd)/build/_li/usr/lib/modules" build/_li/lib/modules
		  depmod -b build/_li "$ver"
		  rm -rf build/netmods && mkdir -p build/netmods; : > build/netmods/order.txt
		  for target in virtio_net virtio_blk ext4; do
		    modprobe -d build/_li -S "$ver" --show-depends "$target" | while read kind ko rest; do
		      [ "$kind" = insmod ] || continue
		      bn=$(basename "${ko%.xz}")
		      grep -qx "$bn" build/netmods/order.txt && continue
		      case "$ko" in
		        *.xz) python3 -c "import lzma,sys;open(sys.argv[2],\"wb\").write(lzma.open(sys.argv[1]).read())" "$ko" "build/netmods/$bn" ;;
		        *) cp "$ko" "build/netmods/$bn" ;;
		      esac
		      echo "$bn" >> build/netmods/order.txt
		    done
		  done
		  rm -rf build/_li
		  echo "staged:"; cat build/netmods/order.txt'
		;;
	fermios)
		drun make all && drun make disk
		echo "[run.sh] booting FermiHV + fermi-os guest (interactive; Ctrl-A X to quit)"
		docker run --rm -it -v "$HERE":/work -w /work "$IMAGE" \
			qemu-system-aarch64 \
			-machine virt,gic-version=3,virtualization=on,highmem=off -cpu cortex-a72 \
			-m 3G -nographic -nic none -kernel build/fermihv.elf \
			-device loader,addr=0x46000000,data=0x0FE33105,data-len=4
		;;
	fermios-raw)
		drun make all
		echo "[run.sh] booting FermiHV + fermi-os guest (captured)..."
		drun bash -c 'timeout 30 qemu-system-aarch64 \
			-machine virt,gic-version=3,virtualization=on,highmem=off -cpu cortex-a72 \
			-m 3G -nographic -nic none -kernel build/fermihv.elf \
			-device loader,addr=0x46000000,data=0x0FE33105,data-len=4 \
			2>&1 || true'
		;;
	fermios-vgic)
		drun make all
		echo "[run.sh] booting fermi-os on the EMULATED vGIC (interactive; Ctrl-A X to quit)"
		docker run --rm -it -v "$HERE":/work -w /work "$IMAGE" \
			qemu-system-aarch64 \
			-machine virt,gic-version=3,virtualization=on,highmem=off -cpu cortex-a72 \
			-m 3G -nographic -nic none -kernel build/fermihv.elf \
			-device loader,addr=0x46000000,data=0x0FE33106,data-len=4
		;;
	fermios-vgic-raw)
		drun make all
		echo "[run.sh] booting fermi-os on the EMULATED vGIC (captured)..."
		drun bash -c 'timeout 30 qemu-system-aarch64 \
			-machine virt,gic-version=3,virtualization=on,highmem=off -cpu cortex-a72 \
			-m 3G -nographic -nic none -kernel build/fermihv.elf \
			-device loader,addr=0x46000000,data=0x0FE33106,data-len=4 \
			2>&1 || true'
		;;
	linux)
		drun make all && drun make disk
		echo "[run.sh] booting FermiHV + Linux guest (interactive; Ctrl-A X to quit)"
		# -it gives the guest's busybox shell a real terminal on the serial console.
		docker run --rm -it -v "$HERE":/work -w /work "$IMAGE" \
			qemu-system-aarch64 \
			-machine virt,gic-version=3,virtualization=on,highmem=off -cpu cortex-a72 \
			-m 3G -nographic -netdev user,id=n0 -device virtio-net-pci,netdev=n0 -drive file=build/disk.img,if=none,id=d0,format=raw -device virtio-blk-pci,drive=d0 -kernel build/fermihv.elf \
			-device loader,file=build/Image,addr=0x41000000,force-raw=on \
			-device loader,file=build/guest.dtb,addr=0x48000000,force-raw=on \
			-device loader,file=build/initramfs.cpio.gz,addr=0x4c000000,force-raw=on
		;;
	linux-demo)
		# Non-interactive: pipe a canned command sequence into the guest shell.
		drun make all && drun make disk
		printf 'uname -a\ncat /proc/cpuinfo | head -6\nls -la /\ncat /proc/interrupts\necho FERMIHV_SHELL_OK\npoweroff -f\n' | \
		docker run --rm -i -v "$HERE":/work -w /work "$IMAGE" bash -c 'timeout 95 qemu-system-aarch64 \
			-machine virt,gic-version=3,virtualization=on,highmem=off -cpu cortex-a72 \
			-m 3G -nographic -netdev user,id=n0 -device virtio-net-pci,netdev=n0 -drive file=build/disk.img,if=none,id=d0,format=raw -device virtio-blk-pci,drive=d0 -kernel build/fermihv.elf \
			-device loader,file=build/Image,addr=0x41000000,force-raw=on \
			-device loader,file=build/guest.dtb,addr=0x48000000,force-raw=on \
			-device loader,file=build/initramfs.cpio.gz,addr=0x4c000000,force-raw=on \
			2>&1 || true'
		;;
	linux-raw)
		drun make all && drun make disk
		echo "[run.sh] booting FermiHV + Linux guest (captured, ${LINUX_TIMEOUT:-95}s)..."
		drun bash -c 'timeout 95 qemu-system-aarch64 \
			-machine virt,gic-version=3,virtualization=on,highmem=off -cpu cortex-a72 \
			-m 3G -nographic -netdev user,id=n0 -device virtio-net-pci,netdev=n0 -drive file=build/disk.img,if=none,id=d0,format=raw -device virtio-blk-pci,drive=d0 -kernel build/fermihv.elf \
			-device loader,file=build/Image,addr=0x41000000,force-raw=on \
			-device loader,file=build/guest.dtb,addr=0x48000000,force-raw=on \
			-device loader,file=build/initramfs.cpio.gz,addr=0x4c000000,force-raw=on \
			2>&1 || true'
		;;
	test)
		drun make all && drun make disk
		echo "[run.sh] booting for 8s, capturing serial..."
		drun bash -c 'timeout 30 qemu-system-aarch64 \
			-machine virt,gic-version=3,virtualization=on,highmem=off -cpu cortex-a72 \
			-m 3G -nographic -netdev user,id=n0 -device virtio-net-pci,netdev=n0 -drive file=build/disk.img,if=none,id=d0,format=raw -device virtio-blk-pci,drive=d0 -kernel build/fermihv.elf 2>&1 || true'
		;;
	*) echo "unknown command: $cmd" >&2; exit 2 ;;
esac
