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
echo "--- / listing ---";       ls -la /
echo "--- guest interrupts ---"; cat /proc/interrupts
echo "FERMIHV_SHELL_OK"
echo
echo "  (interactive: run ./run.sh linux and type; 'poweroff -f' to halt)"
echo

exec /bin/busybox sh
