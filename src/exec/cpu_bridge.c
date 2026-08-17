/*
 * cpu_bridge.c - CPU bridge: services syscalls from the GPU runner.
 *
 * On Linux this dispatches through the raw syscall interface so the
 * semantics match what a native program would observe.  On Windows the
 * same operation numbers map onto the CRT equivalents so the .bbf
 * pipeline is fully functional on both platforms.
 */

#include "cpu_bridge.h"
#include "bine_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Syscall numbers shared by the .bbf ABI (Linux SYS_* values). */
#define BINE_SYS_read   0
#define BINE_SYS_write  1
#define BINE_SYS_open   2
#define BINE_SYS_close  3
#define BINE_SYS_brk    12
#define BINE_SYS_mmap   9
#define BINE_SYS_munmap 11
#define BINE_SYS_exit   60

#if defined(BINE_POSIX)
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <fcntl.h>
#endif

#if defined(BINE_WINDOWS)
#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <sys/stat.h>
#endif

int cpu_bridge_init(void) {
    /* Nothing to do; the bridge is stateless. */
    return 0;
}

void cpu_bridge_shutdown(void) {
    /* Nothing. */
}

int cpu_bridge_invoke(int nr, uint64_t *args, int nargs, int64_t *result) {
    if (!result) return -1;
    long ret = -1;

#if defined(BINE_POSIX)
    switch (nr) {
        case BINE_SYS_read:
            ret = (long)syscall(SYS_read,
                (int)args[0], (void*)args[1], (size_t)args[2]);
            break;
        case BINE_SYS_write:
            ret = (long)syscall(SYS_write,
                (int)args[0], (const void*)args[1], (size_t)args[2]);
            break;
        case BINE_SYS_open:
            ret = (long)syscall(SYS_open,
                (const char*)args[0], (int)args[1], (mode_t)args[2]);
            break;
        case BINE_SYS_close:
            ret = (long)syscall(SYS_close, (int)args[0]);
            break;
        case BINE_SYS_exit:
            _exit((int)args[0]);
            break;
        case BINE_SYS_brk:
            ret = (long)syscall(SYS_brk, (void*)args[0]);
            break;
        case BINE_SYS_mmap:
            ret = (long)syscall(SYS_mmap,
                (void*)args[0], (size_t)args[1], (int)args[2],
                (int)args[3], (int)args[4], (off_t)args[5]);
            break;
        case BINE_SYS_munmap:
            ret = (long)syscall(SYS_munmap, (void*)args[0], (size_t)args[1]);
            break;
        default:
            fprintf(stderr, "cpu_bridge: unsupported syscall %d\n", nr);
            ret = -38; /* -ENOSYS */
            break;
    }
#else
    (void)nargs;
    switch (nr) {
        case BINE_SYS_read:
            ret = (long)_read((int)args[0], (void*)args[1], (unsigned)args[2]);
            break;
        case BINE_SYS_write:
            ret = (long)_write((int)args[0], (const void*)args[1], (unsigned)args[2]);
            break;
        case BINE_SYS_open:
            ret = (long)_open((const char*)args[0],
                              (int)args[1] | _O_BINARY,
                              _S_IREAD | _S_IWRITE);
            break;
        case BINE_SYS_close:
            ret = (long)_close((int)args[0]);
            break;
        case BINE_SYS_exit:
            _exit((int)args[0]);
            break;
        case BINE_SYS_brk:
        case BINE_SYS_mmap:
        case BINE_SYS_munmap:
            /* Not meaningful on Windows; report unsupported. */
            ret = -38;
            break;
        default:
            fprintf(stderr, "cpu_bridge: unsupported syscall %d\n", nr);
            ret = -38;
            break;
    }
#endif
    *result = (int64_t)ret;
    return 0;
}

int cpu_bridge_service(gpu_runner_state_t *state) {
    if (!state) return 0;
    if (!state->syscall_pending) return 0;
    int64_t result = 0;
    cpu_bridge_invoke(state->syscall_nr,
                      (uint64_t*)state->syscall_args, 6, &result);
    state->syscall_result = result;
    state->syscall_pending = 0;
    return 1;
}

int cpu_bridge_run(gpu_runner_state_t *state, int max_iterations) {
    if (!state) return 0;
    int total = 0;
    for (int i = 0; i < max_iterations; i++) {
        if (state->halted) break;
        if (cpu_bridge_service(state)) total++;
    }
    return total;
}
