#!/bin/busybox sh
# PID 1 for the FermiHV Linux guest's busybox initramfs.
/bin/busybox mount -t proc     proc     /proc 2>/dev/null
/bin/busybox mount -t sysfs    sysfs    /sys  2>/dev/null
/bin/busybox mount -t devtmpfs devtmpfs /dev  2>/dev/null
/bin/busybox --install -s /bin 2>/dev/null

echo
echo "  ============================================================"
echo "   busybox shell  --  Linux guest on the FermiHV hypervisor"
echo "  ============================================================"

# Run a few real commands so each boot demonstrates userspace tools working
# on the guest (output is captured by ./run.sh linux-raw).
uname -a
echo "--- nproc / loadavg ---"; nproc; cat /proc/loadavg
echo "--- guest interrupts ---"; cat /proc/interrupts

echo "--- loading virtio_net modules ---"
if [ -f /lib/modules/order.txt ]; then
	while read m; do
		insmod "/lib/modules/$m" 2>/dev/null && echo "  loaded $m" || echo "  (skip $m)"
	done < /lib/modules/order.txt
	sleep 1
fi

echo "--- network bring-up ---"
if [ -e /sys/class/net/eth0 ]; then
	# QEMU usermode (SLIRP) network: guest 10.0.2.15/24, gw 10.0.2.2, dns 10.0.2.3
	ip link set eth0 up
	ip addr add 10.0.2.15/24 dev eth0
	ip route add default via 10.0.2.2
	mkdir -p /etc
	echo "nameserver 10.0.2.3" > /etc/resolv.conf
	ip addr show eth0
	echo "--- DNS lookup via SLIRP (nslookup deb.debian.org) ---"
	nslookup deb.debian.org 2>&1 | head -6
	echo "--- HTTP GET via guest virtio-net (SLIRP NAT) ---"
	wget -T 15 -q -O - http://deb.debian.org/debian/dists/stable/Release 2>&1 | head -6
	echo "--- (networking test done) ---"
else
	echo "[init] no eth0 yet -- available interfaces:"; ls /sys/class/net
	echo "[init] virtio in dmesg:"; dmesg | grep -i "virtio\|eth0" | head
fi
echo "FERMIHV_SHELL_OK"
echo
echo "  (interactive: run ./run.sh linux and type; 'poweroff -f' to halt)"
echo

exec /bin/busybox sh
