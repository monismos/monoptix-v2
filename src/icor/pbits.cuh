// icor/pbits.cuh — P-bit Maximum Independent Set scheduler for GPU-ICOR
// No structs. No classes. Pure CUDA, pure bits.
// Integrates after k_find_pairs, before k_apply_rules.

#ifndef PBITS_CUH
#define PBITS_CUH

#include <curand_kernel.h>
#include "bits.cuh"
#include <cuda_runtime.h>
#include <cstdint>
#include <cstdlib>

#define MAX_CONF 6        // max conflicts per pair (3 ports × 2 sides)
#define PBIT_STEPS  10    // Boltzmann steps per beta level
#define BLOCKS(n)  (((n) + 255) / 256)

// Beta schedule for annealing
static const float BETA_SCHEDULE[] = { 0.3f, 0.7f, 1.5f, 3.0f, 7.0f, 15.0f, 40.0f };
#define N_BETAS  (sizeof(BETA_SCHEDULE)/sizeof(float))

// ────────────────────────────────────────────────────────────────
// PHASE 1 — Build reverse index: agent → which pair uses it
// ────────────────────────────────────────────────────────────────
__global__ void k_agent_to_pair(
    uint32_t *pairs_a,      // in:  pair left  agent indices
    uint32_t *pairs_b,      // in:  pair right agent indices
    uint32_t *agent_pair,   // out: agent_pair[agent_idx] = pair_idx (0xFFFFFFFF = none)
    uint32_t  n_pairs)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n_pairs) return;
    atomicExch(&agent_pair[pairs_a[i]], i);
    atomicExch(&agent_pair[pairs_b[i]], i);
}

// ────────────────────────────────────────────────────────────────
// PHASE 2 — Find conflicts: pairs that share an aux-port neighbor
// ────────────────────────────────────────────────────────────────
__global__ void k_find_conflicts(
    uint32_t *pairs_a, uint32_t *pairs_b,
    uint64_t *net,
    uint32_t *agent_pair,
    uint32_t *conf_adj,    // out: [n_pairs × MAX_CONF] flattened
    uint8_t  *conf_cnt,    // out: conflict count per pair
    uint32_t  n_pairs, uint32_t n)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n_pairs) return;

    uint8_t  cnt = 0;
    uint32_t sides[2] = { pairs_a[i], pairs_b[i] };

    for (int s = 0; s < 2; s++) {
        uint64_t w = net[sides[s]];
        // Auxiliary ports 1 and 2 — port 0 is the principal (already in a pair)
        for (int p = 1; p <= 2; p++) {
            uint32_t tgt = WORD_PORT_ADDR(w, p);
            if (tgt == 0 || tgt >= n) continue;
            uint32_t op = agent_pair[tgt];
            if (op == 0xFFFFFFFF || op == i) continue;
            // Dedup: scan the small list
            bool dup = false;
            for (uint8_t m = 0; m < cnt; m++)
                if (conf_adj[i * MAX_CONF + m] == op) { dup = true; break; }
            if (!dup && cnt < MAX_CONF)
                conf_adj[i * MAX_CONF + cnt++] = op;
        }
    }
    conf_cnt[i] = cnt;
}

// ────────────────────────────────────────────────────────────────
// PHASE 3 — P-bit update kernel (one Boltzmann step)
// 
// Ising formulation:
//   s_i ∈ {-1, +1}   (+1 = include pair in MIS, -1 = exclude)
//   I_i = bias - Σ_{j∈conflicts(i)} s_j
//   m_i = sign( tanh(β·I_i) + r_i ),   r_i ~ U(-1,1)
// 
// Bias = +1.5 for ERA pairs (prefer annihilation over duplication)
// Bias = +1.0 for all others (neutral preference for inclusion)
// ────────────────────────────────────────────────────────────────
__global__ void k_pbit_update(
    int8_t   *s,          // in/out: p-bit states (+1 or -1)
    uint32_t *conf_adj,   // in:     conflict adjacency [n × MAX_CONF]
    uint8_t  *conf_cnt,   // in:     conflict count per pair
    uint8_t  *rule_bias,  // in:     per-pair bias: 2=ERA,1=other
    uint32_t  n_pairs,
    float     beta,
    curandState *rng)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n_pairs) return;

    curandState lcl = rng[i];

    // Bias: ERA pairs (annihilate → prefer inclusion) get extra +0.5
    float I = (rule_bias[i] == 2) ? 1.5f : 1.0f;

    uint8_t nc = conf_cnt[i];
    for (uint8_t k = 0; k < nc; k++)
        I -= (float)s[conf_adj[i * MAX_CONF + k]];

    float r = curand_uniform(&lcl) * 2.0f - 1.0f;
    s[i] = (tanhf(beta * I) + r > 0.0f) ? (int8_t)1 : (int8_t)-1;

    rng[i] = lcl;
}

// ────────────────────────────────────────────────────────────────
// PHASE 4 — Determine rule bias per pair (ERA annihilates → prefer)
// ────────────────────────────────────────────────────────────────
__global__ void k_rule_bias(
    uint32_t *pairs_a, uint32_t *pairs_b,
    uint64_t *net,
    uint8_t  *rule_bias,
    uint32_t n_pairs)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n_pairs) return;
    uint64_t wa = net[pairs_a[i]];
    uint64_t wb = net[pairs_b[i]];
    uint32_t ta = WORD_TYPE(wa), tb = WORD_TYPE(wb);
    // ERA×ERA or X×ERA: annihilation, strongly prefer
    rule_bias[i] = (ta == 2 || tb == 2) ? 2 : 1;
}

// ────────────────────────────────────────────────────────────────
// PHASE 5 — Initialize cuRAND states
// ────────────────────────────────────────────────────────────────
__global__ void k_init_rng(curandState *rng, unsigned long long seed, uint32_t n) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    curand_init(seed, i, 0, &rng[i]);
}

// ────────────────────────────────────────────────────────────────
// PHASE 6 — Compact MIS output into a dense apply-list
// ────────────────────────────────────────────────────────────────
__global__ void k_compress_mis(
    int8_t   *s,           // p-bit states
    uint32_t *pairs_a_in,  uint32_t *pairs_b_in,
    uint32_t *pairs_a_out, uint32_t *pairs_b_out,
    uint32_t *scatter,     // exclusive prefix sum of (s[i]==1)
    uint32_t  n_pairs)
{
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n_pairs || s[i] != 1) return;
    uint32_t pos = scatter[i];
    pairs_a_out[pos] = pairs_a_in[i];
    pairs_b_out[pos] = pairs_b_in[i];
}

// Kernel to convert int8 flags to uint8 for prefix sum
__global__ void k_convert_flags(int8_t *s, uint8_t *flags, uint32_t n) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    flags[i] = (s[i] == 1) ? 1u : 0u;
}

static uint32_t gpu_excl_sum_u8(uint8_t* d_flags, uint32_t* d_scatter, uint32_t n) {
    uint8_t* h_flags = (uint8_t*)malloc(n);
    uint32_t* h_scatter = (uint32_t*)malloc(sizeof(uint32_t) * n);
    if (!h_flags || !h_scatter) {
        free(h_flags);
        free(h_scatter);
        return 0;
    }

    cudaMemcpy(h_flags, d_flags, n, cudaMemcpyDeviceToHost);
    uint32_t running = 0;
    for (uint32_t i = 0; i < n; i++) {
        h_scatter[i] = running;
        running += h_flags[i] ? 1u : 0u;
    }
    cudaMemcpy(d_scatter, h_scatter, sizeof(uint32_t) * n, cudaMemcpyHostToDevice);

    free(h_flags);
    free(h_scatter);
    return running;
}

// ────────────────────────────────────────────────────────────────
// HOST ORCHESTRATION — runs the full p-bit MIS pipeline
// Returns n_mis (the size of the selected conflict-free subset)
// ────────────────────────────────────────────────────────────────
uint32_t pbit_mis(
    uint32_t *d_pairs_a, uint32_t *d_pairs_b,
    uint64_t *d_net,
    uint32_t *d_pairs_a_out, uint32_t *d_pairs_b_out,
    curandState *d_rng,
    uint32_t n_pairs, uint32_t n_agents)
{
    if (n_pairs == 0) return 0;

    // Allocate working arrays
    uint32_t *d_agent_pair, *d_conf_adj, *d_scatter;
    uint8_t  *d_conf_cnt, *d_rule_bias, *d_flags;
    int8_t   *d_s;
    cudaMalloc(&d_agent_pair, n_agents * 4);
    cudaMalloc(&d_conf_adj,   n_pairs * MAX_CONF * 4);
    cudaMalloc(&d_conf_cnt,   n_pairs);
    cudaMalloc(&d_rule_bias,  n_pairs);
    cudaMalloc(&d_s,          n_pairs);
    cudaMalloc(&d_scatter,    n_pairs * 4);
    cudaMalloc(&d_flags,      n_pairs);

    // 1. Initialize: every agent → no pair
    cudaMemset(d_agent_pair, 0xFF, n_agents * 4);

    // 2. Build agent→pair reverse index
    k_agent_to_pair<<<BLOCKS(n_pairs), 256>>>(
        d_pairs_a, d_pairs_b, d_agent_pair, n_pairs);
    cudaDeviceSynchronize();

    // 3. Find conflicts
    k_find_conflicts<<<BLOCKS(n_pairs), 256>>>(
        d_pairs_a, d_pairs_b, d_net, d_agent_pair,
        d_conf_adj, d_conf_cnt, n_pairs, n_agents);
    cudaDeviceSynchronize();

    // 4. Compute rule bias
    k_rule_bias<<<BLOCKS(n_pairs), 256>>>(
        d_pairs_a, d_pairs_b, d_net, d_rule_bias, n_pairs);
    cudaDeviceSynchronize();

    // 5. Initialize p-bits: all +1 (optimistic — try to include everything)
    cudaMemset(d_s, 1, n_pairs);

    // 6. Simulated annealing: β from low (hot/random) → high (greedy/frozen)
    for (int b = 0; b < (int)N_BETAS; b++) {
        for (int step = 0; step < PBIT_STEPS; step++) {
            k_pbit_update<<<BLOCKS(n_pairs), 256>>>(
                d_s, d_conf_adj, d_conf_cnt, d_rule_bias,
                n_pairs, BETA_SCHEDULE[b], d_rng);
        }
    }
    cudaDeviceSynchronize();

    // 7. Compact: gather all pairs where s[i] == +1
    k_convert_flags<<<BLOCKS(n_pairs), 256>>>(d_s, d_flags, n_pairs);
    cudaDeviceSynchronize();

    uint32_t n_mis = gpu_excl_sum_u8(d_flags, d_scatter, n_pairs);

    k_compress_mis<<<BLOCKS(n_pairs), 256>>>(
        d_s, d_pairs_a, d_pairs_b,
        d_pairs_a_out, d_pairs_b_out,
        d_scatter, n_pairs);
    cudaDeviceSynchronize();

    cudaFree(d_agent_pair); cudaFree(d_conf_adj); cudaFree(d_conf_cnt);
    cudaFree(d_rule_bias);  cudaFree(d_s);        cudaFree(d_scatter);
    cudaFree(d_flags);
    return n_mis;
}

#endif // PBITS_CUH
