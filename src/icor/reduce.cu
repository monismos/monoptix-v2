// icor/reduce.c — host orchestration. C, not C++. No classes.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <cuda_runtime.h>
#include "compact.cuh"
#include "hashcons.cuh"
#include "pbits.cuh"

// External CUDA kernels (defined in kernels.cu)
__global__ void k_find_pairs(uint64_t* net, uint32_t n_agents, 
                         uint64_t* out_pairs, uint32_t* out_count);
__global__ void k_apply_rules(uint64_t* net, uint64_t* pairs, uint32_t pair_count,
                         uint32_t* alloc_head, uint64_t* out_pairs, uint32_t* out_count);
__global__ void k_mark_live(uint64_t* net, uint32_t n, uint32_t* flags);
__global__ void k_scatter(uint64_t* net, uint32_t* prefix, uint32_t* flags,
                      uint64_t* out_net, uint32_t* remap, uint32_t n);
__global__ void k_remap_ports(uint64_t* net, uint32_t* remap, uint32_t n);
__global__ void k_compute_hashes(uint64_t* net, uint64_t* hashes, uint32_t* frontier,
                             uint32_t frontier_n, uint32_t* next_frontier, uint32_t* next_count);

// Compact the bit array to remove dead words
static uint32_t compact_bit_array(uint64_t* d_net, uint32_t n_agents) {
    return compact_bit_array_gpu(d_net, n_agents);
}

// Hashcons to fixpoint - collapse identical agents (full deterministic implementation)
void hashcons_to_fixpoint(uint64_t* d_net, uint32_t n_agents) {
    hashcons_collapse(d_net, &n_agents);
}

// Helper kernel: unpack packed pairs (a_id << 32 | b_id) into separate arrays
__global__ void k_unpack_pairs(uint64_t *pairs_packed, uint32_t *pairs_a, uint32_t *pairs_b, uint32_t n) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    uint64_t packed = pairs_packed[i];
    pairs_a[i] = (uint32_t)(packed >> 32);
    pairs_b[i] = (uint32_t)packed;
}

// Helper kernel: repack separate arrays into packed format
__global__ void k_repack_pairs(uint32_t *pairs_a, uint32_t *pairs_b, uint64_t *pairs_packed, uint32_t n) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    pairs_packed[i] = ((uint64_t)pairs_a[i] << 32) | (uint64_t)pairs_b[i];
}

// Main reduction function with p-bit MIS
extern "C" void icor_reduce(uint64_t* d_net, uint32_t* d_alloc_head,
                 uint32_t  n_agents, uint32_t* final_count)
{
    
    // Pair queues: two flat uint64_t arrays (ping-pong)
    uint64_t *d_pairs_A, *d_pairs_B;
    uint32_t *d_count_A, *d_count_B;
    cudaMalloc((void**)&d_pairs_A, sizeof(uint64_t) * n_agents);
    cudaMalloc((void**)&d_pairs_B, sizeof(uint64_t) * n_agents);
    cudaMalloc((void**)&d_count_A, sizeof(uint32_t));
    cudaMalloc((void**)&d_count_B, sizeof(uint32_t));

    // Unpacked pair arrays for MIS scheduler
    uint32_t *d_pairs_a, *d_pairs_b;
    uint32_t *d_pairs_a_mis, *d_pairs_b_mis;
    cudaMalloc(&d_pairs_a, n_agents * 4);
    cudaMalloc(&d_pairs_b, n_agents * 4);
    cudaMalloc(&d_pairs_a_mis, n_agents * 4);
    cudaMalloc(&d_pairs_b_mis, n_agents * 4);

    uint32_t grid_agents = (n_agents + 255) / 256;

    // Round 0: find all initial active pairs
    cudaMemset(d_count_A, 0, 4);
    k_find_pairs<<<grid_agents, 256>>>(d_net, n_agents, d_pairs_A, d_count_A);
    cudaDeviceSynchronize();

    uint32_t round = 0;
    uint32_t max_rounds = 1000;  // Safety limit
    
    while (1) {
        uint32_t active;
        cudaMemcpy(&active, d_count_A, 4, cudaMemcpyDeviceToHost);
        
        if (active == 0) break;

        // Progress reporting every 50 rounds
        if (round % 50 == 0) {
            printf("[GPU-ICOR] Round %u: %u active pairs, %zu bytes\n", round, active, n_agents * sizeof(uint64_t));
            fflush(stdout);
        }

        // Always use p-bit MIS scheduler (complete implementation)
        // Unpack pairs for MIS scheduler
        __global__ void k_unpack_pairs(uint64_t *pairs_packed, uint32_t *pairs_a, uint32_t *pairs_b, uint32_t n);
        k_unpack_pairs<<<(active + 255) / 256, 256>>>(d_pairs_A, d_pairs_a, d_pairs_b, active);
        cudaDeviceSynchronize();

        // Run p-bit MIS to find conflict-free subset
        uint32_t n_mis = pbit_mis(
            d_pairs_a, d_pairs_b, d_net,
            d_pairs_a_mis, d_pairs_b_mis,
            active, n_agents);

        // Apply rules only on MIS pairs (zero conflicts)
        cudaMemset(d_count_B, 0, 4);
        uint32_t grid_mis = (n_mis + 127) / 128;
        
        // Repack MIS pairs for k_apply_rules
        __global__ void k_repack_pairs(uint32_t *pairs_a, uint32_t *pairs_b, uint64_t *pairs_packed, uint32_t n);
        k_repack_pairs<<<grid_mis, 256>>>(d_pairs_a_mis, d_pairs_b_mis, d_pairs_B, n_mis);
        cudaDeviceSynchronize();

        k_apply_rules<<<grid_mis, 128>>>(
            d_net, d_pairs_B, n_mis, d_alloc_head, d_pairs_A, d_count_B
        );
        cudaDeviceSynchronize();

        // Ping-pong
        uint64_t* tmp_p = d_pairs_A; d_pairs_A = d_pairs_B; d_pairs_B = tmp_p;
        uint32_t* tmp_c = d_count_A; d_count_A = d_count_B; d_count_B = tmp_c;

        // Compact every 8 rounds with hashcons
        if (++round % 8 == 0) {
            uint32_t new_n = compact_bit_array(d_net, n_agents);
            if (new_n == n_agents && active < 10) {
                // No change in agent count and few active pairs - near fixpoint
                printf("[GPU-ICOR] Near fixpoint at round %u, exiting early\n", round);
                break;
            }
            n_agents = new_n;
            // Run hashcons collapse after compaction
            hashcons_to_fixpoint(d_net, n_agents);
            // Update n_agents after hashcons
            n_agents = compact_bit_array(d_net, n_agents);
        }

        // Safety limit to prevent infinite loops
        if (round >= max_rounds) {
            printf("[GPU-ICOR] Reached max rounds (%u), exiting\n", max_rounds);
            break;
        }
    }

    // Final compact with hashcons
    n_agents = compact_bit_array(d_net, n_agents);
    hashcons_to_fixpoint(d_net, n_agents);
    n_agents = compact_bit_array(d_net, n_agents);
    *final_count = n_agents;

    cudaFree(d_pairs_A);
    cudaFree(d_pairs_B);
    cudaFree(d_count_A);
    cudaFree(d_count_B);
    cudaFree(d_pairs_a);
    cudaFree(d_pairs_b);
    cudaFree(d_pairs_a_mis);
    cudaFree(d_pairs_b_mis);
}
