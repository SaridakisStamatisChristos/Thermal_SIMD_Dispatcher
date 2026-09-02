#define _GNU_SOURCE
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

/*
 * test_thermal_simd uses synthetic perf descriptors 100/101 to exercise the
 * production EINTR/partial-read machinery. Production now correctly requires
 * RESET/ENABLE to succeed before baseline validation, so this executable-local
 * interposer makes only those synthetic descriptors ioctl-capable. Every real
 * descriptor is forwarded to the kernel unchanged.
 */
int ioctl(int fd, unsigned long request, ...) {
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);

    if (fd == 100 || fd == 101) {
        return 0;
    }
    return (int)syscall(SYS_ioctl, fd, request, arg);
}
