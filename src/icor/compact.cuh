// icor/compact.cuh — deterministic compaction helper

#ifndef ICOR_COMPACT_CUH
#define ICOR_COMPACT_CUH

#include <cuda_runtime.h>
#include <cstdint>
#include <cstdlib>

static void gpu_excl_sum(uint32_t *d_flags,   /* in:  1=live, 0=dead  */
                         uint32_t *d_scatter,  /* out: exclusive prefix sum */
                         uint32_t  n,
                         uint32_t *h_total)    /* out: total live count */
{
    uint32_t* h_flags = (uint32_t*)malloc(sizeof(uint32_t) * n);
    uint32_t* h_scatter = (uint32_t*)malloc(sizeof(uint32_t) * n);
    if (!h_flags || !h_scatter) {
        free(h_flags);
        free(h_scatter);
        *h_total = 0;
        return;
    }

    cudaMemcpy(h_flags, d_flags, sizeof(uint32_t) * n, cudaMemcpyDeviceToHost);
    uint32_t running = 0;
    for (uint32_t i = 0; i < n; i++) {
        h_scatter[i] = running;
        running += h_flags[i];
    }
    cudaMemcpy(d_scatter, h_scatter, sizeof(uint32_t) * n, cudaMemcpyHostToDevice);
    *h_total = running;

    free(h_flags);
    free(h_scatter);
}

// Compact the bit array to remove dead words (GPU-side)
static uint32_t compact_bit_array_gpu(uint64_t* d_net, uint32_t n_agents) {
    uint32_t* d_flags;
    uint32_t* d_prefix;
    uint32_t* d_remap;
    uint64_t* d_out_net;
    cudaMalloc(&d_flags, sizeof(uint32_t) * n_agents);
    cudaMalloc(&d_prefix, sizeof(uint32_t) * n_agents);
    cudaMalloc(&d_remap, sizeof(uint32_t) * n_agents);
    cudaMalloc(&d_out_net, sizeof(uint64_t) * n_agents);

    // Step 1: Mark live agents
    uint32_t grid_agents = (n_agents + 255) / 256;
    
    // Use the existing k_mark_live kernel from kernels.cu
    extern __global__ void k_mark_live(uint64_t* net, uint32_t n, uint32_t* flags);
    k_mark_live<<<grid_agents, 256>>>(d_net, n_agents, d_flags);
    cudaDeviceSynchronize();

    // Step 2: deterministic prefix sum for scatter addresses
    uint32_t live_count;
    gpu_excl_sum(d_flags, d_prefix, n_agents, &live_count);

    // Step 3: Scatter
    extern __global__ void k_scatter(uint64_t* net, uint32_t* prefix, uint32_t* flags,
                                     uint64_t* out_net, uint32_t* remap, uint32_t n);
    k_scatter<<<grid_agents, 256>>>(d_net, d_prefix, d_flags, d_out_net, d_remap, n_agents);
    cudaDeviceSynchronize();

    // Step 4: Remap ports
    extern __global__ void k_remap_ports(uint64_t* net, uint32_t* remap, uint32_t n);
    k_remap_ports<<<grid_agents, 256>>>(d_out_net, d_remap, live_count);
    cudaDeviceSynchronize();

    // Copy back to original net
    cudaMemcpy(d_net, d_out_net, sizeof(uint64_t) * live_count, cudaMemcpyDeviceToDevice);

    cudaFree(d_flags);
    cudaFree(d_prefix);
    cudaFree(d_remap);
    cudaFree(d_out_net);

    return live_count;
}

#endif // ICOR_COMPACT_CUH
