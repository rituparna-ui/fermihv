/*
 * Minimal static /init for the Linux guest's initramfs.
 *
 * Statically linked, no libc (own _start, raw aarch64 syscalls). Linux execs
 * it as PID 1. It opens /dev/console, announces that userspace is alive on
 * FermiHV, then heartbeats forever (PID 1 must never exit or the kernel
 * panics with "Attempted to kill init").
 */
#define SYS_openat   56
#define SYS_write    64
#define SYS_nanosleep 101

static long syscall3(long n, long a, long b, long c) {
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
	return x0;
}

static int slen(const char *s) {
	int n = 0;
	while (s[n])
		n++;
	return n;
}

void _start(void) {
	const char *banner =
	    "\n"
	    "========================================================\n"
	    "  Hello from Linux USERSPACE (PID 1 /init) on FermiHV!\n"
	    "  A from-scratch EL2 hypervisor is hosting this kernel.\n"
	    "========================================================\n";
	const char *beat = "[init] userspace heartbeat on FermiHV\n";

	/* AT_FDCWD = -100, O_WRONLY = 1 */
	long fd = syscall3(SYS_openat, -100, (long)"/dev/console", 1);
	long out = (fd >= 0) ? fd : 1;

	syscall3(SYS_write, out, (long)banner, slen(banner));

	long ts[2] = {2, 0}; /* 2 seconds */
	for (;;) {
		syscall3(SYS_nanosleep, (long)ts, 0, 0);
		syscall3(SYS_write, out, (long)beat, slen(beat));
	}
}
