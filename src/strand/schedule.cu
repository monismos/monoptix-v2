// bine/strand/schedule_p_full.cu — STRAND-P Full Implementation
// Probabilistic instruction scheduling using p-bits and Ising model

#include "bine.h"
#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

// External p-bit state machine functions
extern void strand_p_init_pbits(PBitState* d_pbits, int n, float initial_temp, uint64_t seed);
extern void strand_p_calculate_field(const float* d_couplings, const float* d_biases,
                                      const float* d_spins, float* d_local_field, int n);
extern void strand_p_update_pbits(PBitState* d_pbits, const float* d_local_field,
                                  float* d_spins, int n, float current_temp, uint64_t seed);
extern float strand_p_calculate_energy(const float* d_couplings, const float* d_biases,
                                       const float* d_spins, int n);
extern void strand_p_anneal_temperature(PBitState* d_pbits, int n,
                                       float temp_start, float temp_end,
                                       int current_step, int total_steps);
extern void strand_p_tapsa_update(PBitState* d_pbits, float* d_time_avg_spins,
                                  const float* d_current_spins, int n, float alpha);
extern bool strand_p_detect_stall(const float* d_energy_history, int history_len,
                                   float stall_threshold);

// STRAND-P scheduler state
typedef struct {
    PBitState* d_pbits;           // Device p-bit states
    float* d_couplings;           // Coupling matrix
    float* d_biases;              // External biases
    float* d_spins;               // Current spin values
    float* d_local_field;         // Local field for each spin
    float* d_time_avg_spins;      // Time-averaged spins (TApSA)
    float* d_energy_history;     // Energy history for stall detection
    int n_instructions;           // Number of instructions to schedule
    int n_spins;                  // Number of p-bits (one per instruction)
    float temp_start;             // Initial temperature
    float temp_end;               // Final temperature
    int max_iterations;           // Maximum annealing iterations
    float tapsa_alpha;             // TApSA smoothing factor
    float stall_threshold;        // Energy variance threshold for stall
} STRANDPScheduler;

// Initialize STRAND-P scheduler
int strand_p_init(STRANDPScheduler* scheduler, int n_instructions,
                  const uint64_t* instructions, const uint8_t* masks_r,
                  const uint8_t* masks_w, const uint8_t* masks_k) {
    memset(scheduler, 0, sizeof(STRANDPScheduler));
    
    scheduler->n_instructions = n_instructions;
    scheduler->n_spins = n_instructions;
    scheduler->temp_start = 2.0f;
    scheduler->temp_end = 0.01f;
    scheduler->max_iterations = 1000;
    scheduler->tapsa_alpha = 0.1f;
    scheduler->stall_threshold = 1e-6f;
    
    // Allocate device memory
    cudaMalloc(&scheduler->d_pbits, n_instructions * sizeof(PBitState));
    cudaMalloc(&scheduler->d_couplings, n_instructions * n_instructions * sizeof(float));
    cudaMalloc(&scheduler->d_biases, n_instructions * sizeof(float));
    cudaMalloc(&scheduler->d_spins, n_instructions * sizeof(float));
    cudaMalloc(&scheduler->d_local_field, n_instructions * sizeof(float));
    cudaMalloc(&scheduler->d_time_avg_spins, n_instructions * sizeof(float));
    cudaMalloc(&scheduler->d_energy_history, 100 * sizeof(float));
    
    // Initialize p-bits
    strand_p_init_pbits(scheduler->d_pbits, n_instructions, scheduler->temp_start, 0x9e3779b97f4a7c15ull);
    
    // Build coupling matrix from dependency analysis
    // J[i][j] > 0 if instruction i must come before j (antiferromagnetic)
    // J[i][j] < 0 if instructions can be parallel (ferromagnetic)
    float* h_couplings = (float*)malloc(n_instructions * n_instructions * sizeof(float));
    float* h_biases = (float*)malloc(n_instructions * sizeof(float));
    
    for (int i = 0; i < n_instructions; i++) {
        h_biases[i] = 0.0f;  // No external bias initially
        for (int j = 0; j < n_instructions; j++) {
            h_couplings[i * n_instructions + j] = 0.0f;
        }
    }
    
    // Analyze dependencies using masks
    for (int i = 0; i < n_instructions; i++) {
        for (int j = i + 1; j < n_instructions; j++) {
            // Check for RAW, WAR, WAW conflicts
            bool raw = (masks_r[i] & masks_w[j]) != 0;
            bool war = (masks_w[i] & masks_r[j]) != 0;
            bool waw = (masks_w[i] & masks_w[j]) != 0;
            
            if (raw || war || waw) {
                // Dependency exists: antiferromagnetic coupling
                // Positive coupling encourages opposite spins (ordering)
                h_couplings[i * n_instructions + j] = 1.0f;
                h_couplings[j * n_instructions + i] = 1.0f;
            } else {
                // No dependency: ferromagnetic coupling
                // Negative coupling encourages same spins (parallelism)
                h_couplings[i * n_instructions + j] = -0.5f;
                h_couplings[j * n_instructions + i] = -0.5f;
            }
        }
    }
    
    // Copy to device
    cudaMemcpy(scheduler->d_couplings, h_couplings,
               n_instructions * n_instructions * sizeof(float), cudaMemcpyHostToDevice);
    cudaMemcpy(scheduler->d_biases, h_biases,
               n_instructions * sizeof(float), cudaMemcpyHostToDevice);
    
    free(h_couplings);
    free(h_biases);
    
    return 0;
}

// Run STRAND-P optimization
int strand_p_optimize(STRANDPScheduler* scheduler, uint32_t* schedule_order) {
    
    float current_temp = scheduler->temp_start;
    float best_energy = INFINITY;
    int stall_count = 0;
    
    for (int iter = 0; iter < scheduler->max_iterations; iter++) {
        // Calculate local field
        strand_p_calculate_field(scheduler->d_couplings, scheduler->d_biases,
                                 scheduler->d_spins, scheduler->d_local_field,
                                 scheduler->n_spins);
        
        // Update p-bits
        strand_p_update_pbits(scheduler->d_pbits, scheduler->d_local_field,
                              scheduler->d_spins, scheduler->n_spins, current_temp,
                              0x9e3779b97f4a7c15ull + (uint64_t)iter);
        
        // Calculate energy
        float energy = strand_p_calculate_energy(scheduler->d_couplings, scheduler->d_biases,
                                                 scheduler->d_spins, scheduler->n_spins);
        
        // Update energy history
        if (iter < 100) {
            cudaMemcpy(scheduler->d_energy_history + iter, &energy, sizeof(float),
                       cudaMemcpyHostToDevice);
        }
        
        // Track best energy
        if (energy < best_energy) {
            best_energy = energy;
            stall_count = 0;
        } else {
            stall_count++;
        }
        
        // TApSA: update time-averaged spins
        strand_p_tapsa_update(scheduler->d_pbits, scheduler->d_time_avg_spins,
                               scheduler->d_spins, scheduler->n_spins,
                               scheduler->tapsa_alpha);
        
        // SpSA: check for convergence
        if (iter > 50) {
            bool stalled = strand_p_detect_stall(scheduler->d_energy_history, 50,
                                                  scheduler->stall_threshold);
            if (stalled) {
                break;
            }
        }
        
        // Anneal temperature
        strand_p_anneal_temperature(scheduler->d_pbits, scheduler->n_spins,
                                    scheduler->temp_start, scheduler->temp_end,
                                    iter, scheduler->max_iterations);
        current_temp = scheduler->temp_start * 
                       powf(scheduler->temp_end / scheduler->temp_start,
                            (float)iter / scheduler->max_iterations);
        
        if (iter % 100 == 0) {
        }
    }
    
    // Extract final schedule from time-averaged spins
    float* h_spins = (float*)malloc(scheduler->n_spins * sizeof(float));
    cudaMemcpy(h_spins, scheduler->d_time_avg_spins,
               scheduler->n_spins * sizeof(float), cudaMemcpyDeviceToHost);
    
    // Sort instructions by spin value (positive spins first)
    for (int i = 0; i < scheduler->n_spins; i++) {
        schedule_order[i] = i;
    }
    
    // Simple insertion sort by spin value
    for (int i = 1; i < scheduler->n_spins; i++) {
        int j = i;
        while (j > 0 && h_spins[schedule_order[j]] > h_spins[schedule_order[j-1]]) {
            uint32_t temp = schedule_order[j];
            schedule_order[j] = schedule_order[j-1];
            schedule_order[j-1] = temp;
            j--;
        }
    }
    
    free(h_spins);
    
    return 0;
}

// Free STRAND-P scheduler
void strand_p_free(STRANDPScheduler* scheduler) {
    if (scheduler->d_pbits) cudaFree(scheduler->d_pbits);
    if (scheduler->d_couplings) cudaFree(scheduler->d_couplings);
    if (scheduler->d_biases) cudaFree(scheduler->d_biases);
    if (scheduler->d_spins) cudaFree(scheduler->d_spins);
    if (scheduler->d_local_field) cudaFree(scheduler->d_local_field);
    if (scheduler->d_time_avg_spins) cudaFree(scheduler->d_time_avg_spins);
    if (scheduler->d_energy_history) cudaFree(scheduler->d_energy_history);
    memset(scheduler, 0, sizeof(STRANDPScheduler));
}

// C interface for integration
extern "C" {

int strand_p_schedule_instructions(uint32_t* schedule_order, int n_instructions,
                                    const uint64_t* instructions,
                                    const uint8_t* masks_r, const uint8_t* masks_w,
                                    const uint8_t* masks_k) {
    STRANDPScheduler scheduler;
    
    if (strand_p_init(&scheduler, n_instructions, instructions,
                      masks_r, masks_w, masks_k) != 0) {
        return -1;
    }
    
    int result = strand_p_optimize(&scheduler, schedule_order);
    
    strand_p_free(&scheduler);
    
    return result;
}

// C-callable wrapper for STRAND-P optimization
int bine_strand_p_optimize_wrapper(const uint64_t* net, uint32_t n_agents,
                                    const uint8_t* masks_r, const uint8_t* masks_w,
                                    const uint8_t* masks_k,
                                    uint64_t** scheduled_net, uint32_t* n_scheduled) {
    
    // Allocate schedule order
    uint32_t* schedule_order = (uint32_t*)malloc(n_agents * sizeof(uint32_t));
    
    // Run STRAND-P scheduling
    int result = strand_p_schedule_instructions(schedule_order, n_agents, net,
                                               masks_r, masks_w, masks_k);
    
    if (result != 0) {
        free(schedule_order);
        return -1;
    }
    
    // Reorder net according to schedule
    uint64_t* scheduled = (uint64_t*)malloc(n_agents * sizeof(uint64_t));
    for (uint32_t i = 0; i < n_agents; i++) {
        scheduled[i] = net[schedule_order[i]];
    }
    
    free(schedule_order);
    
    *scheduled_net = scheduled;
    *n_scheduled = n_agents;
    
    return 0;
}

} // extern "C"
