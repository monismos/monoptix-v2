/*
 * test_gpu.c - GPU engine smoke tests (GPU-ICOR reduction + STRAND-P p-bit
 * scheduling).  Only built when CUDA is enabled; skips cleanly when no CUDA
 * device is present (CI runners without GPUs).
 */

#ifndef BINE_BUILD_GPU
#error "test_gpu.c requires BINE_BUILD_GPU (CUDA build)"
#endif

#include "bine.h"
#include "icor/bits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int bine_gpu_icor_reduce_wrapper(const uint64_t* h_net, uint32_t n_agents,
                                        uint64_t** out_net, uint32_t* out_n,
                                        uint32_t* rounds);
extern int bine_strand_p_optimize_wrapper(const uint64_t* net, uint32_t n_agents,
                                          const uint8_t* masks_r, const uint8_t* masks_w,
                                          const uint8_t* masks_k,
                                          uint64_t** scheduled_net, uint32_t* n_scheduled);
extern int cudaGetDeviceCount(int* count);

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int test_gpu_main(void) {
    printf("=== GPU engines (CUDA) ===\n");
    int dev_count = 0;
    if (cudaGetDeviceCount(&dev_count) != 0 || dev_count <= 0) {
        printf("test_gpu: SKIPPED (no CUDA device)\n");
        return 0;
    }
    printf("  device available: %d\n", dev_count);

    /* Active pair: ERA -> agent 1, CON -> agent 0. */
    uint64_t net[2];
    net[0] = SET_PORT(SET_TYPE(0, 2), 0, PORTREF(1, 0));
    net[1] = SET_PORT(SET_TYPE(0, 0), 0, PORTREF(0, 0));

    /* GPU-ICOR: must terminate with a sane reduced net. */
    uint64_t* out = NULL;
    uint32_t n_out = 0, rounds = 0;
    ASSERT(bine_gpu_icor_reduce_wrapper(net, 2, &out, &n_out, &rounds) == 0);
    ASSERT(out != NULL && n_out <= 2);
    uint32_t live = 0;
    for (uint32_t i = 0; i < n_out; i++) {
        ASSERT(WORD_TYPE(out[i]) <= 4);
        if (!WORD_DEAD(out[i])) live++;
    }
    ASSERT(live <= 1); /* an ERA-CON pair leaves at most one live agent */
    printf("  GPU-ICOR: 2 -> %u agents OK\n", n_out);
    free(out);

    /* STRAND-P: must return a valid schedule of all agents. */
    uint8_t mr[2] = { 1, 1 };
    uint8_t mw[2] = { 1, 1 };
    uint8_t mk[2] = { 0, 0 };
    uint64_t* sched = NULL;
    uint32_t n_sched = 0;
    ASSERT(bine_strand_p_optimize_wrapper(net, 2, mr, mw, mk, &sched, &n_sched) == 0);
    ASSERT(sched != NULL && n_sched == 2);
    printf("  GPU-STRAND-P: %u agents scheduled OK\n", n_sched);
    free(sched);

    printf("test_gpu: all tests PASSED\n");
    return 0;
}

#ifndef BINE_COMBINED_TESTS
int main(void) { return test_gpu_main(); }
#endif