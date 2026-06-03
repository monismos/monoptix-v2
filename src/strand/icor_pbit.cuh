// icor/icor_pbit.cuh — True P-bit annealing for GPU-ICOR reduction

#ifndef ICOR_PBIT_CUH
#define ICOR_PBIT_CUH

#include "bits.cuh"
#include "rules.cuh"
#include "compact.cuh"
#include <cuda_runtime.h>
#include "rng.cuh"
#include <cstdint>

// Find pairs kernel - same as deterministic version
extern __global__ void k_find_pairs(uint64_t* net, uint32_t n_agents, 
                                   uint64_t* out_pairs, uint32_t* out_count);

// Apply rules kernel - same as deterministic version
extern __global__ void k_apply_rules(uint64_t* __restrict__ net,
                                     uint64_t* __restrict__ pairs,
                                     uint32_t               pair_count,
                                     uint32_t* __restrict__ alloc_head,
                                     uint64_t* __restrict__ out_pairs,
                                     uint32_t* __restrict__ out_count);

// P-bit rule application kernel with annealing
// Each p-bit decides whether to apply its rule based on temperature and local state
__global__ void k_apply_rules_pbit(
    uint64_t* __restrict__ net,
    uint64_t* __restrict__ pairs,
    uint32_t               pair_count,
    uint32_t* __restrict__ alloc_head,
    uint64_t* __restrict__ out_pairs,
    uint32_t* __restrict__ out_count,
    float                  temperature,
    uint64_t               rng_seed)
{
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= pair_count) return;

    uint64_t pair = pairs[tid];
    uint32_t a = (uint32_t)(pair >> 32);
    uint32_t b = (uint32_t)pair;

    uint64_t wa = net[a];
    uint64_t wb = net[b];

    // Stale check
    if (WORD_DEAD(wa) || WORD_DEAD(wb)) return;
    if (!WORD_AP(wa)) return;

    // SplitMix64 RNG
    uint64_t rng = splitmix64_seed(rng_seed, tid);
    
    // Compute energy contribution of this pair
    uint8_t ta = WORD_TYPE(wa);
    uint8_t tb = WORD_TYPE(wb);
    
    float energy_change = 0.0f;
    if (ta == 2 && tb == 2) {
        energy_change = -2.0f;  // ERA×ERA: removes 2 agents
    } else if (ta == 2 || tb == 2) {
        energy_change = -1.0f;  // X×ERA: removes 1 agent
    } else if (ta == tb) {
        energy_change = 0.0f;   // CON×CON or DUP×DUP: neutral
    } else {
        energy_change = 2.0f;   // CON×DUP: adds 2 agents
    }
    
    // Metropolis acceptance criterion
    float accept_prob = 1.0f;
    if (energy_change > 0.0f) {
        accept_prob = expf(-energy_change / temperature);
    }
    
    float r = splitmix64_uniform(&rng);
    if (r > accept_prob) {
        return; // Reject
    }

    // Clear AP bits
    atomicAnd((unsigned long long*)(net + a), ~0x2ULL);
    atomicAnd((unsigned long long*)(net + b), ~0x2ULL);

    // Apply the rule
    if (ta == 2 && tb == 2) {
        rule_era_era(net, a, b);
    } else if (ta == 2 || tb == 2) {
        uint32_t x   = (ta == 2) ? b : a;
        uint32_t era = (ta == 2) ? a : b;
        uint32_t fresh = atomicAdd(alloc_head, 1u);
        rule_x_era(net, x, era, fresh);
    } else if (ta == tb) {
        if (ta == 0) rule_con_con(net, a, b);
        else         rule_dup_dup(net, a, b);
    } else {
        uint32_t con = (ta == 0) ? a : b;
        uint32_t dup = (ta == 0) ? b : a;
        uint32_t base = atomicAdd(alloc_head, 4u);
        rule_con_dup(net, con, dup, base);
    }
    
    // Emit new pairs
    uint32_t slot = atomicAdd(out_count, 1u);
    out_pairs[slot] = pair;
}

// k_init_rng is defined in pbits.cuh - use that one
extern __global__ void k_init_rng(curandState *rng, unsigned long long seed, uint32_t n);

// Temperature schedule for annealing
static const float TEMPERATURE_SCHEDULE[] = { 10.0f, 5.0f, 2.0f, 1.0f, 0.5f, 0.2f, 0.1f };
#define N_TEMPS (sizeof(TEMPERATURE_SCHEDULE)/sizeof(float))

// P-bit accelerated GPU-ICOR reduction with true annealing
void icor_reduce_pbit(uint64_t* d_net, uint32_t* d_alloc_head,
                     uint32_t  n_agents, uint32_t* final_count)
{
    uint64_t *d_pairs_A, *d_pairs_B;
    uint32_t *d_count_A, *d_count_B;
    cudaMalloc(&d_pairs_A, sizeof(uint64_t) * n_agents);
    cudaMalloc(&d_pairs_B, sizeof(uint64_t) * n_agents);
    cudaMalloc(&d_count_A, sizeof(uint32_t));
    cudaMalloc(&d_count_B, sizeof(uint32_t));

    // Deterministic seed for SplitMix64 RNG (no cuRAND state needed)
    uint64_t rng_seed = 0x9e3779b97f4a7c15ull; // golden ratio hash

    uint32_t grid_agents = (n_agents + 255) / 256;

    // Round 0: find all initial active pairs
    cudaMemset(d_count_A, 0, 4);
    k_find_pairs<<<grid_agents, 256>>>(d_net, n_agents, d_pairs_A, d_count_A);
    cudaDeviceSynchronize();

    uint32_t round = 0;
    uint32_t max_rounds = 1000;  // Safety limit
    uint32_t temp_idx = 0;
    
    while (1) {
        uint32_t active;
        cudaMemcpy(&active, d_count_A, 4, cudaMemcpyDeviceToHost);
        if (active == 0) break;

        // Progress reporting every 50 rounds
        if (round % 50 == 0) {
            printf("[GPU-ICOR-PBIT] Round %u: %u active pairs, %u agents, temp=%.2f\n", 
                   round, active, n_agents, TEMPERATURE_SCHEDULE[temp_idx]);
            fflush(stdout);
        }

        // Get current temperature
        float temperature = TEMPERATURE_SCHEDULE[temp_idx];
        if (temp_idx < N_TEMPS - 1) temp_idx++;

        // Apply rules with p-bit annealing
        cudaMemset(d_count_B, 0, 4);
        uint32_t grid_pairs = (active + 127) / 128;
        k_apply_rules_pbit<<<grid_pairs, 128>>>(
            d_net, d_pairs_A, active, d_alloc_head, d_pairs_B, d_count_B,
            temperature, rng_seed + round);
        cudaDeviceSynchronize();

        // Ping-pong
        uint64_t* tmp_p = d_pairs_A; d_pairs_A = d_pairs_B; d_pairs_B = tmp_p;
        uint32_t* tmp_c = d_count_A; d_count_A = d_count_B; d_count_B = tmp_c;

        // Compact every 8 rounds
        if (++round % 8 == 0) {
            uint32_t new_n = compact_bit_array_gpu(d_net, n_agents);
            if (new_n == n_agents && active < 10) {
                printf("[GPU-ICOR-PBIT] Near fixpoint at round %u, exiting early\n", round);
                break;
            }
            n_agents = new_n;
            grid_agents = (n_agents + 255) / 256;
        }

        // Safety limit
        if (round >= max_rounds) {
            printf("[GPU-ICOR-PBIT] Reached max rounds (%u), exiting\n", max_rounds);
            break;
        }
    }

    // Final compact
    n_agents = compact_bit_array_gpu(d_net, n_agents);
    
    // Hashcons collapse
    extern void hashcons_collapse(uint64_t *d_net, uint32_t *n_inout);
    hashcons_collapse(d_net, &n_agents);
    n_agents = compact_bit_array_gpu(d_net, n_agents);
    
    *final_count = n_agents;

    cudaFree(d_pairs_A);
    cudaFree(d_pairs_B);
    cudaFree(d_count_A);
    cudaFree(d_count_B);
}

#endif // ICOR_PBIT_CUH
