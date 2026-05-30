/*
 * STRAND P-Bit Probabilistic Scheduling Kernels
 * 
 * GPU-accelerated P-bit (probabilistic bit) magnetization for STRAND v2
 * Converts the deterministic P-bit refinement to CUDA kernels
 */

#ifndef STRAND_PBIT_CUH
#define STRAND_PBIT_CUH

#include <cuda_runtime.h>
#include <stdint.h>
#include <math.h>

#define PBIT_TEMPERATURE_STAGES 4
#define PBIT_MAX_SWEEPS 16
#define PBIT_INHIBITION_COEFF 0.35f

// Temperature schedule for P-bit annealing
__constant__ float PBIT_TEMPERATURES[PBIT_TEMPERATURE_STAGES] = {1.75f, 1.1f, 0.7f, 0.45f};

// P-bit state for each node
typedef struct {
    float magnetization;
    float base_score;
    uint32_t priority;
    uint32_t release_count;
    float transition_cost;
    uint32_t original_rank;
} PBitNodeState;

// =============================================================================
// Kernel 1: Initialize P-Bit States
// =============================================================================
__global__ void k_init_pbit_states(
    const uint32_t* __restrict__ priorities,
    const uint32_t* __restrict__ release_counts,
    const uint32_t* __restrict__ original_ranks,
    const float* __restrict__ transition_costs,
    const uint32_t node_count,
    PBitNodeState* __restrict__ states
) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= node_count) return;
    
    states[tid].priority = priorities[tid];
    states[tid].release_count = release_counts[tid];
    states[tid].original_rank = original_ranks[tid];
    states[tid].transition_cost = transition_costs[tid];
    
    // Compute base score
    float alpha = 1.0f;  // Priority weight (computed from std dev on host)
    float beta = 1.0f;   // Cost weight
    float gamma = 1.0f;  // Release weight
    
    states[tid].base_score = 
        alpha * (float)priorities[tid]
        - beta * transition_costs[tid]
        + gamma * (float)release_counts[tid];
    
    // Initialize magnetization from base score
    states[tid].magnetization = tanhf(states[tid].base_score);
}

// =============================================================================
// Kernel 2: Compute Pair Penalties (conflict costs between ready nodes)
// =============================================================================
__global__ void k_compute_pair_penalties(
    const uint32_t* __restrict__ ready_nodes,
    const uint32_t ready_count,
    const uint64_t* __restrict__ kill_words,
    float* __restrict__ penalties  // Flattened matrix: ready_count x ready_count
) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t total_pairs = ready_count * ready_count;
    
    if (tid >= total_pairs) return;
    
    uint32_t i = tid / ready_count;
    uint32_t j = tid % ready_count;
    
    if (i == j) {
        penalties[tid] = 0.0f;
        return;
    }
    
    // Compute transition cost between two nodes
    uint64_t wi = kill_words[ready_nodes[i]];
    uint64_t wj = kill_words[ready_nodes[j]];
    
    uint32_t cost_ij = __popcll(wi ^ wj);
    uint32_t cost_ji = cost_ij;  // Symmetric for XOR
    
    penalties[tid] = (float)(cost_ij + cost_ji);
}

// =============================================================================
// Kernel 3: P-Bit Magnetization Sweep
// =============================================================================
__global__ void k_pbit_sweep(
    const uint32_t* __restrict__ ready_nodes,
    const uint32_t ready_count,
    const PBitNodeState* __restrict__ states,
    const float* __restrict__ pair_penalties,
    float temperature,
    PBitNodeState* __restrict__ new_states
) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= ready_count) return;
    
    uint32_t node_idx = ready_nodes[tid];
    PBitNodeState state = states[node_idx];
    
    // Compute inhibition from other ready nodes
    float inhibition = 0.0f;
    for (uint32_t j = 0; j < ready_count; j++) {
        if (j == tid) continue;
        
        uint32_t other_idx = ready_nodes[j];
        float other_mag = states[other_idx].magnetization;
        float penalty = pair_penalties[tid * ready_count + j];
        
        inhibition += (1.0f + penalty) * other_mag;
    }
    
    // Compute local field
    float field = state.base_score - PBIT_INHIBITION_COEFF * inhibition;
    
    // Update magnetization
    float new_mag = tanhf(field / temperature);
    
    new_states[node_idx] = state;
    new_states[node_idx].magnetization = new_mag;
}

// =============================================================================
// Kernel 4: Select Best Ready Node
// =============================================================================
__global__ void k_select_best_ready(
    const uint32_t* __restrict__ ready_nodes,
    const uint32_t ready_count,
    const PBitNodeState* __restrict__ states,
    const uint32_t* __restrict__ current_ranks,
    uint32_t* __restrict__ best_node
) {
    if (ready_count == 0) return;
    if (ready_count == 1) {
        *best_node = ready_nodes[0];
        return;
    }
    
    // Shared memory for reduction
    __shared__ float shared_mags[256];
    __shared__ uint32_t shared_indices[256];
    
    uint32_t tid = threadIdx.x;
    
    // Initialize with worst values
    float best_mag = -FLT_MAX;
    uint32_t best_idx = ready_nodes[0];
    
    // Each thread processes multiple elements
    for (uint32_t i = tid; i < ready_count; i += blockDim.x) {
        uint32_t node = ready_nodes[i];
        float mag = states[node].magnetization;
        float base = states[node].base_score;
        uint32_t priority = states[node].priority;
        uint32_t release = states[node].release_count;
        uint32_t rank = current_ranks[node];
        
        // Compare nodes
        bool is_better = false;
        if (fabsf(mag - best_mag) > 1e-6f) {
            is_better = mag > best_mag;
        } else if (fabsf(base - states[best_idx].base_score) > 1e-6f) {
            is_better = base > states[best_idx].base_score;
        } else if (priority != states[best_idx].priority) {
            is_better = priority > states[best_idx].priority;
        } else if (release != states[best_idx].release_count) {
            is_better = release > states[best_idx].release_count;
        } else {
            is_better = rank < states[best_idx].original_rank;
        }
        
        if (is_better) {
            best_mag = mag;
            best_idx = node;
        }
    }
    
    shared_mags[tid] = best_mag;
    shared_indices[tid] = best_idx;
    __syncthreads();
    
    // Reduce in shared memory
    for (uint32_t s = blockDim.x / 2; s > 0; s >>= 1) {
        if (tid < s) {
            if (shared_mags[tid + s] > shared_mags[tid]) {
                shared_mags[tid] = shared_mags[tid + s];
                shared_indices[tid] = shared_indices[tid + s];
            }
        }
        __syncthreads();
    }
    
    if (tid == 0) {
        *best_node = shared_indices[0];
    }
}

// =============================================================================
// Kernel 5: Compute Move Delta (for refinement)
// =============================================================================
__global__ void k_compute_move_delta(
    const uint32_t* __restrict__ schedule,
    const uint32_t schedule_len,
    const uint32_t src_pos,
    const uint32_t dst_pos,
    const uint64_t* __restrict__ masked_words,
    int64_t* __restrict__ out_delta
) {
    if (src_pos == dst_pos) {
        *out_delta = 0;
        return;
    }
    
    uint32_t node = schedule[src_pos];
    
    // Get neighbors at source position
    uint64_t prev_a = (src_pos > 0) ? masked_words[schedule[src_pos - 1]] : 0;
    uint64_t next_a = (src_pos + 1 < schedule_len) ? masked_words[schedule[src_pos + 1]] : 0;
    uint64_t node_w = masked_words[node];
    
    // Get neighbors at destination position
    uint64_t prev_b, next_b;
    if (dst_pos < src_pos) {
        prev_b = (dst_pos > 0) ? masked_words[schedule[dst_pos - 1]] : 0;
        next_b = masked_words[schedule[dst_pos]];
    } else {
        prev_b = masked_words[schedule[dst_pos]];
        next_b = (dst_pos + 1 < schedule_len) ? masked_words[schedule[dst_pos + 1]] : 0;
    }
    
    // Compute old cost (around source)
    uint32_t old_cost = __popcll(prev_a ^ node_w);
    if (src_pos + 1 < schedule_len) {
        old_cost += __popcll(node_w ^ next_a);
    }
    
    // Compute new cost
    uint32_t new_cost = __popcll(prev_b ^ node_w);
    new_cost += __popcll(node_w ^ next_b);
    
    *out_delta = (int64_t)new_cost - (int64_t)old_cost;
}

// =============================================================================
// Host Functions
// =============================================================================

// Run P-bit scheduling on GPU
void strand_schedule_gpu(
    const uint64_t* d_words,
    const uint32_t* d_priorities,
    const uint32_t n_words,
    const SchedulingGraph* graph,
    const uint64_t mk,
    uint32_t* out_schedule
);

// Run P-bit refinement on GPU  
void strand_pbit_refine_gpu(
    const uint64_t* d_words,
    uint32_t* d_schedule,
    uint32_t n_words,
    const SchedulingGraph* graph,
    const uint64_t mk,
    uint32_t max_sweeps,
    uint32_t* out_sweeps_used,
    uint32_t* out_moves_applied
);

// Compute transition cost on GPU
uint64_t compute_schedule_cost_gpu(
    const uint32_t* d_schedule,
    const uint64_t* d_masked_words,
    uint32_t n_words
);

#endif // STRAND_PBIT_CUH
