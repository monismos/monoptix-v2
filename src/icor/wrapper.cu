#include "bine.h"
#include "bits.cuh"
#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" void icor_reduce(uint64_t* d_net, uint32_t* d_alloc_head,
                            uint32_t n_agents, uint32_t* final_count);

extern "C" int bine_gpu_icor_reduce_wrapper(const uint64_t* h_net, uint32_t n_agents,
                                       uint64_t** out_net, uint32_t* out_n) {
    uint64_t *d_net, *d_result;
    uint32_t *d_alloc_head, final_count, alloc_head = n_agents + 65536;
    uint32_t max_agents = n_agents + 262144;

    cudaMalloc(&d_net, max_agents * sizeof(uint64_t));
    cudaMalloc(&d_alloc_head, sizeof(uint32_t));
    cudaMalloc(&d_result, max_agents * sizeof(uint64_t));
    cudaMemcpy(d_net, h_net, n_agents * sizeof(uint64_t), cudaMemcpyHostToDevice);
    cudaMemcpy(d_alloc_head, &alloc_head, sizeof(uint32_t), cudaMemcpyHostToDevice);

    icor_reduce(d_net, d_alloc_head, n_agents, &final_count);
    cudaDeviceSynchronize();

    uint64_t* h_out = (uint64_t*)malloc(final_count * sizeof(uint64_t));
    cudaMemcpy(h_out, d_net, final_count * sizeof(uint64_t), cudaMemcpyDeviceToHost);

    *out_net = h_out;
    *out_n = final_count;

    cudaFree(d_net);
    cudaFree(d_alloc_head);
    cudaFree(d_result);
    return 0;
}
