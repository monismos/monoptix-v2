/*
 * gpu_runner.h - Execute a reduced net directly on the GPU
 *
 * The reduced net is the optimized program.  We don't translate back
 * to x86-64; we execute the net as a graph of CUDA threads.
 *
 * Each node = one CUDA thread (or warp for high fanout).
 * Wires = values passed via shared memory or global memory.
 * Synchronization = per-step barrier (BFS level in net).
 *
 * I/O requests are flagged via a special "syscall" node; the CPU bridge
 * polls a flag in pinned memory and services the request.
 */

#ifndef BINE_GPU_RUNNER_H
#define BINE_GPU_RUNNER_H

#include "bbf.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Inputs from CPU */
    uint64_t *input_values;   /* one per input port */
    uint32_t  num_inputs;
    /* Outputs to CPU */
    uint64_t *output_values;  /* one per output port */
    uint32_t  num_outputs;
    /* Memory for STORE/LOAD semantics */
    uint8_t  *memory;         /* flat address space */
    uint64_t  memory_size;
    /* Syscall request: CPU polls this and services */
    volatile int syscall_nr;
    volatile uint64_t syscall_args[6];
    volatile int syscall_pending;
    volatile int64_t syscall_result;
    /* Halt flag: set by HALT node */
    volatile int halted;
} gpu_runner_state_t;

int  gpu_runner_state_init(gpu_runner_state_t *s, const bine_net_t *net,
                            size_t memory_size);
void gpu_runner_state_free(gpu_runner_state_t *s);

/* Execute the net.  Returns 0 on success, non-zero on error. */
int  gpu_runner_execute(const bine_net_t *net, gpu_runner_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* BINE_GPU_RUNNER_H */
