// bine/strand/schedule_p.cu — Stage 5: STRAND-P Joint Optimization
// P-bit annealing for globally optimal instruction schedule + register pressure.

#include "bine.h"
#include <cuda_runtime.h>
#include <device_launch_parameters.h>
#include <curand_kernel.h>
#include <stdio.h>

// P-bit state: m_i ∈ {-1, +1}
// +1 = schedule early (high priority)
// -1 = schedule late (low priority)

struct PBitState {
    int* m;          // p-bit array [-1, +1]
    float* J;        // Coupling matrix J_ij
    float* h;        // Bias array h_i
    uint32_t n;      // Number of agents
};

// Custom popcount that works on both host and device
__host__ __device__ uint32_t popcount64(uint64_t x) {
    uint32_t count = 0;
    while (x) {
        count += x & 1;
        x >>= 1;
    }
    return count;
}

// Compute transition cost T(A_i, A_j) = popcount((A_i XOR A_j) & M_K)
__host__ __device__ float transition_cost(uint64_t a, uint64_t b, __uint128_t M_K) {
    uint64_t xor_val = a ^ b;
    uint64_t masked = xor_val & (M_K & 0xFFFFFFFFFFFFFFFFULL);
    return popcount64(masked);
}

// Compute priority P(A_i) = popcount(A_i & M_W)
__host__ __device__ float priority(uint64_t a, __uint128_t M_W) {
    uint64_t masked = a & (M_W & 0xFFFFFFFFFFFFFFFFULL);
    return popcount64(masked);
}

// GPU kernel: initialize p-bits from build ID (deterministic seed)
__global__ void k_init_pbits(int* m, uint32_t n, uint64_t seed) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= n) return;
    
    // Use seed + tid to generate deterministic p-bit
    uint64_t hash = seed + tid;
    hash = ((hash >> 32) ^ hash) * 0x9E3779B97F4A7C15ULL;
    hash = ((hash >> 32) ^ hash) * 0x9E3779B97F4A7C15ULL;
    m[tid] = (hash & 1) ? 1 : -1;
}

// GPU kernel: compute Hamiltonian H = -Σ J_ij m_i m_j - Σ h_i m_i
__global__ void k_compute_hamiltonian(int* m, float* J, float* h,
                                      uint32_t n, float* H_out) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= n) return;
    
    float local_H = 0.0f;
    
    // Coupling term: -Σ J_ij m_i m_j
    for (uint32_t j = 0; j < n; j++) {
        if (tid != j) {
            uint32_t idx = tid * n + j;
            local_H -= J[idx] * m[tid] * m[j];
        }
    }
    
    // Bias term: -h_i m_i
    local_H -= h[tid] * m[tid];
    
    atomicAdd(H_out, local_H);
}

// GPU kernel: single p-bit flip with Metropolis acceptance
__global__ void k_pbit_flip(int* m, float* J, float* h, uint32_t n,
                             float temperature, uint64_t* rng_state,
                             uint32_t* flips_accepted) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= n) return;
    
    // Random number generation (simple LCG)
    uint64_t state = rng_state[tid];
    state = state * 1103515245ULL + 12345ULL;
    rng_state[tid] = state;
    float rand_val = (float)(state & 0xFFFFFF) / (float)0xFFFFFF;
    
    // Compute energy change if we flip this p-bit
    float delta_E = 0.0f;
    for (uint32_t j = 0; j < n; j++) {
        if (tid != j) {
            uint32_t idx = tid * n + j;
            delta_E += 2.0f * J[idx] * m[tid] * m[j];
        }
    }
    delta_E += 2.0f * h[tid] * m[tid];
    
    // Metropolis acceptance
    if (delta_E < 0.0f || rand_val < expf(-delta_E / temperature)) {
        m[tid] = -m[tid]; // Flip
        atomicAdd(flips_accepted, 1);
    }
}

// GPU kernel: sort agents by p-bit value (schedule order)
__global__ void k_sort_by_pbit(int* m, uint64_t* net, uint64_t* sorted_net,
                                uint32_t n) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= n) return;
    
    // Count how many have m = +1 before this agent
    uint32_t pos_plus = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (m[i] == 1 && i < tid) pos_plus++;
    }
    
    // Count how many have m = -1 before this agent
    uint32_t pos_minus = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (m[i] == -1 && i < tid) pos_minus++;
    }
    
    // Position in sorted order: all +1 first, then all -1
    uint32_t sorted_pos;
    if (m[tid] == 1) {
        sorted_pos = pos_plus;
    } else {
        // Count total +1 to know offset
        uint32_t total_plus = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (m[i] == 1) total_plus++;
        }
        sorted_pos = total_plus + pos_minus;
    }
    
    sorted_net[sorted_pos] = net[tid];
}

// Main STRAND-P optimization function
uint64_t* bine_strand_p_optimize(const uint64_t* net, uint32_t n_agents,
                                 const StrandMasks* masks,
                                 uint32_t* n_scheduled) {
    if (!net || n_agents == 0 || !masks) {
        *n_scheduled = 0;
        return NULL;
    }
    
    // Allocate device memory
    int* d_m;
    float* d_J;
    float* d_h;
    uint64_t* d_net;
    uint64_t* d_sorted_net;
    uint64_t* d_rng_state;
    uint32_t* d_flips_accepted;
    
    cudaMalloc(&d_m, n_agents * sizeof(int));
    cudaMalloc(&d_J, n_agents * n_agents * sizeof(float));
    cudaMalloc(&d_h, n_agents * sizeof(float));
    cudaMalloc(&d_net, n_agents * sizeof(uint64_t));
    cudaMalloc(&d_sorted_net, n_agents * sizeof(uint64_t));
    cudaMalloc(&d_rng_state, n_agents * sizeof(uint64_t));
    cudaMalloc(&d_flips_accepted, sizeof(uint32_t));
    
    // Copy net to device
    cudaMemcpy(d_net, net, n_agents * sizeof(uint64_t), cudaMemcpyHostToDevice);
    
    // Initialize p-bits from build ID (deterministic)
    uint64_t seed = 0xDEADBEEF; // Would use actual build ID
    uint32_t threads = 256;
    uint32_t blocks = (n_agents + threads - 1) / threads;
    k_init_pbits<<<blocks, threads>>>(d_m, n_agents, seed);
    
    // Initialize RNG state
    uint64_t* h_rng = (uint64_t*)malloc(n_agents * sizeof(uint64_t));
    for (uint32_t i = 0; i < n_agents; i++) {
        h_rng[i] = seed + i;
    }
    cudaMemcpy(d_rng_state, h_rng, n_agents * sizeof(uint64_t), cudaMemcpyHostToDevice);
    free(h_rng);
    
    // Compute coupling matrix J_ij = T(A_i, A_j)
    // and bias h_i = P(A_i)
    float* h_J = (float*)malloc(n_agents * n_agents * sizeof(float));
    float* h_h = (float*)malloc(n_agents * sizeof(float));
    
    for (uint32_t i = 0; i < n_agents; i++) {
        h_h[i] = priority(net[i], masks->M_W);
        
        for (uint32_t j = 0; j < n_agents; j++) {
            if (i != j) {
                h_J[i * n_agents + j] = transition_cost(net[i], net[j], masks->M_K);
            } else {
                h_J[i * n_agents + j] = 0.0f;
            }
        }
    }
    
    cudaMemcpy(d_J, h_J, n_agents * n_agents * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(d_h, h_h, n_agents * sizeof(float), cudaMemcpyHostToDevice);
    free(h_J);
    free(h_h);
    
    // P-bit annealing
    uint32_t max_iterations = 1000;
    float initial_temp = 1.0f;
    float final_temp = 0.01f;
    
    for (uint32_t iter = 0; iter < max_iterations; iter++) {
        float temp = initial_temp * powf(final_temp / initial_temp, (float)iter / max_iterations);
        
        cudaMemset(d_flips_accepted, 0, sizeof(uint32_t));
        
        k_pbit_flip<<<blocks, threads>>>(d_m, d_J, d_h, n_agents, temp,
                                       d_rng_state, d_flips_accepted);
        
        uint32_t flips = 0;
        cudaMemcpy(&flips, d_flips_accepted, sizeof(uint32_t), cudaMemcpyDeviceToHost);
        
        if (flips == 0) break; // Convergence
    }
    
    // Sort agents by p-bit value to get schedule
    k_sort_by_pbit<<<blocks, threads>>>(d_m, d_net, d_sorted_net, n_agents);
    
    // Copy result back
    uint64_t* scheduled_net = (uint64_t*)malloc(n_agents * sizeof(uint64_t));
    cudaMemcpy(scheduled_net, d_sorted_net, n_agents * sizeof(uint64_t), cudaMemcpyDeviceToHost);
    
    *n_scheduled = n_agents;
    
    // Cleanup
    cudaFree(d_m);
    cudaFree(d_J);
    cudaFree(d_h);
    cudaFree(d_net);
    cudaFree(d_sorted_net);
    cudaFree(d_rng_state);
    cudaFree(d_flips_accepted);
    
    
    return scheduled_net;
}
