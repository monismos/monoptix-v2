// bine/strand/pbit_state.cu — P-Bit State Machine for STRAND-P
// Probabilistic bit emulation in CUDA for Ising model optimization

#include "bine.h"
#include <cuda_runtime.h>
#include "rng.cuh"

// Ising model coupling matrix
struct IsingModel {
    float* couplings;      // Coupling matrix J[i][j] (symmetric)
    float* biases;         // External field biases h[i]
    int n_spins;           // Number of spins (p-bits)
    float energy;          // Current system energy
};

// Initialize p-bit states
__global__ void init_pbits_kernel(PBitState* pbits, int n, float initial_temp,
                                   uint64_t seed) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        uint64_t rng = splitmix64_seed(seed, idx);
        pbits[idx].value = splitmix64_uniform(&rng) > 0.5f ? 1.0f : -1.0f;
        pbits[idx].temperature = initial_temp;
        pbits[idx].bias = 0.0f;
        pbits[idx].seed = idx;
    }
}

// Calculate local field for each p-bit
// H_i = sum_j(J_ij * s_j) + h_i
__global__ void calculate_local_field_kernel(const float* couplings, const float* biases,
                                              const float* spins, float* local_field,
                                              int n_spins) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n_spins) {
        float field = biases[i];
        
        // Sum couplings: H_i = sum(J_ij * s_j)
        for (int j = 0; j < n_spins; j++) {
            if (i != j) {
                field += couplings[i * n_spins + j] * spins[j];
            }
        }
        
        local_field[i] = field;
    }
}

// Update p-bit state using Boltzmann distribution
// P(s_i = +1) = 1 / (1 + exp(-2 * H_i / T))
__global__ void update_pbits_kernel(PBitState* pbits, const float* local_field,
                                     float* spins, int n, float current_temp,
                                     uint64_t seed) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        uint64_t rng = splitmix64_seed(seed, idx + n);
        
        float H = local_field[idx];
        float T = current_temp;
        
        // Avoid division by zero
        if (T < 1e-6f) T = 1e-6f;
        
        // Boltzmann probability for spin-up
        float prob_up = 1.0f / (1.0f + expf(-2.0f * H / T));
        
        // Sample from distribution
        float rand_val = splitmix64_uniform(&rng);
        int new_spin = (rand_val < prob_up) ? 1 : -1;
        
        // Update state
        spins[idx] = (float)new_spin;
        pbits[idx].value = (float)new_spin;
        pbits[idx].temperature = T;
    }
}

// Calculate Ising model energy
// E = -0.5 * sum_ij(J_ij * s_i * s_j) - sum_i(h_i * s_i)
__global__ void calculate_energy_kernel(const float* couplings, const float* biases,
                                        const float* spins, float* energy, int n_spins) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n_spins) {
        float local_energy = -biases[i] * spins[i];
        
        for (int j = 0; j < n_spins; j++) {
            if (i < j) {  // Count each pair once
                local_energy -= 0.5f * couplings[i * n_spins + j] * spins[i] * spins[j];
            }
        }
        
        atomicAdd(energy, local_energy);
    }
}

// Temperature annealing schedule (exponential cooling)
__global__ void update_temperature_kernel(PBitState* pbits, int n,
                                          float temp_start, float temp_end,
                                          int current_step, int total_steps) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        // Exponential cooling: T = T_start * (T_end/T_start)^(step/total)
        float cooling_rate = (float)current_step / (float)total_steps;
        float new_temp = temp_start * powf(temp_end / temp_start, cooling_rate);
        
        pbits[idx].temperature = new_temp;
    }
}

// Time-averaged annealing (TApSA) - maintain running average
__global__ void tapsa_update_kernel(PBitState* pbits, float* time_avg_spins,
                                     const float* current_spins, int n,
                                     float alpha) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < n) {
        // Exponential moving average: avg = alpha * current + (1-alpha) * avg
        time_avg_spins[idx] = alpha * current_spins[idx] + (1.0f - alpha) * time_avg_spins[idx];
    }
}

// Stalled annealing detection (SpSA)
__global__ void detect_stall_kernel(const float* energy_history, int history_len,
                                     bool* stalled, float stall_threshold) {
    int idx = threadIdx.x;
    if (idx == 0) {
        // Calculate variance in recent energy history
        if (history_len < 2) {
            *stalled = false;
            return;
        }
        
        float mean = 0.0f;
        for (int i = 0; i < history_len; i++) {
            mean += energy_history[i];
        }
        mean /= history_len;
        
        float variance = 0.0f;
        for (int i = 0; i < history_len; i++) {
            float diff = energy_history[i] - mean;
            variance += diff * diff;
        }
        variance /= history_len;
        
        *stalled = (variance < stall_threshold);
    }
}

// CUDA interface functions
void strand_p_init_pbits(PBitState* d_pbits, int n, float initial_temp, uint64_t seed) {
    int threads = 256;
    int blocks = (n + threads - 1) / threads;
    init_pbits_kernel<<<blocks, threads>>>(d_pbits, n, initial_temp, seed);
    cudaDeviceSynchronize();
}

void strand_p_calculate_field(const float* d_couplings, const float* d_biases,
                                const float* d_spins, float* d_local_field, int n) {
    int threads = 256;
    int blocks = (n + threads - 1) / threads;
    calculate_local_field_kernel<<<blocks, threads>>>(d_couplings, d_biases, d_spins,
                                                       d_local_field, n);
    cudaDeviceSynchronize();
}

void strand_p_update_pbits(PBitState* d_pbits, const float* d_local_field,
                            float* d_spins, int n, float current_temp,
                            uint64_t seed) {
    int threads = 256;
    int blocks = (n + threads - 1) / threads;
    update_pbits_kernel<<<blocks, threads>>>(d_pbits, d_local_field, d_spins, n, current_temp, seed);
    cudaDeviceSynchronize();
}

float strand_p_calculate_energy(const float* d_couplings, const float* d_biases,
                                const float* d_spins, int n) {
    float h_energy = 0.0f;
    float* d_energy;
    cudaMalloc(&d_energy, sizeof(float));
    cudaMemcpy(d_energy, &h_energy, sizeof(float), cudaMemcpyHostToDevice);
    
    int threads = 256;
    int blocks = (n + threads - 1) / threads;
    calculate_energy_kernel<<<blocks, threads>>>(d_couplings, d_biases, d_spins,
                                                   d_energy, n);
    cudaDeviceSynchronize();
    
    cudaMemcpy(&h_energy, d_energy, sizeof(float), cudaMemcpyDeviceToHost);
    cudaFree(d_energy);
    
    return h_energy;
}

void strand_p_anneal_temperature(PBitState* d_pbits, int n,
                                  float temp_start, float temp_end,
                                  int current_step, int total_steps) {
    int threads = 256;
    int blocks = (n + threads - 1) / threads;
    update_temperature_kernel<<<blocks, threads>>>(d_pbits, n, temp_start, temp_end,
                                                   current_step, total_steps);
    cudaDeviceSynchronize();
}

void strand_p_tapsa_update(PBitState* d_pbits, float* d_time_avg_spins,
                            const float* d_current_spins, int n, float alpha) {
    int threads = 256;
    int blocks = (n + threads - 1) / threads;
    tapsa_update_kernel<<<blocks, threads>>>(d_pbits, d_time_avg_spins, d_current_spins, n, alpha);
    cudaDeviceSynchronize();
}

bool strand_p_detect_stall(const float* d_energy_history, int history_len,
                            float stall_threshold) {
    bool h_stalled = false;
    bool* d_stalled;
    cudaMalloc(&d_stalled, sizeof(bool));
    
    detect_stall_kernel<<<1, 1>>>(d_energy_history, history_len, d_stalled, stall_threshold);
    cudaDeviceSynchronize();
    
    cudaMemcpy(&h_stalled, d_stalled, sizeof(bool), cudaMemcpyDeviceToHost);
    cudaFree(d_stalled);
    
    return h_stalled;
}
