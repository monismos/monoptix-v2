/*
 * cpu_bridge.h - CPU bridge for syscalls
 *
 * The GPU runs the optimized net.  When a SYSCALL node fires, the GPU
 * sets a flag in pinned memory.  The CPU polls this flag and services
 * the request, then writes the result back.
 *
 * Syscall numbers follow the Linux x86-64 convention.
 */

#ifndef BINE_CPU_BRIDGE_H
#define BINE_CPU_BRIDGE_H

#include "gpu_runner.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int cpu_bridge_init(void);
void cpu_bridge_shutdown(void);

/* Service any pending syscalls.  Returns 1 if it serviced one, 0 if none. */
int cpu_bridge_service(gpu_runner_state_t *state);

/* Service until halted.  Returns total syscalls served. */
int cpu_bridge_run(gpu_runner_state_t *state, int max_iterations);

/* Direct invocation (bypasses GPU).  Useful for tests. */
int cpu_bridge_invoke(int nr, uint64_t *args, int nargs, int64_t *result);

#ifdef __cplusplus
}
#endif

#endif /* BINE_CPU_BRIDGE_H */
