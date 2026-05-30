/*
 * GPU-STRAND-ICOR: Merged Scheduling and Reduction Kernel
 * 
 * This header merges GPU-ICOR interaction net reduction with STRAND scheduling
 * into a unified GPU pipeline that runs both operations in a single pass.
 * 
 * The key insight: STRAND scheduling can be interleaved with ICOR reduction
 * to minimize scheduling cost at each reduction step, rather than only at the end.
 */

#ifndef STRAND_ICOR_MERGED_CUH
#define STRAND_ICOR_MERGED_CUH

#include <cuda_runtime.h>
#include <stdint.h>
#include "bits.cuh"
#include "strand_scheduling.cuh"
#include "strand_pbit.cuh"

// Maximum iterations for merged reduction-scheduling loop
#define MAX_MERGED_ITERATIONS 1000
#define MIN_AGENTS_THRESHOLD 2

// State for the merged algorithm
typedef struct {
    uint32_t iteration;
    uint32_t total_interactions;
    uint32_t schedule_cost;
    uint32_t converged;
    float reduction_rate;
} MergedAlgorithmState;

// =============================================================================
// Kernel: Interleaved Reduction and Scheduling
// 
// This kernel performs one iteration of:
// 1. Find active pairs in current net
// 2. Apply reduction rules
// 3. Update conflict graph based on new net state
// 4. Run one step of STRAND scheduling on ready nodes
// 5. Select next node to reduce based on scheduling policy
// =============================================================================
__global__ void k_interleaved_reduce_and_schedule(
    uint64_t* __restrict__ net,
    uint32_t* __restrict__ n_agents,
    uint32_t* __restrict__ alloc_head,
    
    // STRAND scheduling data
    uint64_t mr, uint64_t mw, uint64_t mk,
    uint32_t theta_flow, uint32_t theta_anti, uint32_t theta_out,
    
    // Conflict graph (updated each iteration)
    ConflictEdge* __restrict__ conflict_edges,
    uint32_t* __restrict__ conflict_count,
    uint32_t* __restrict__ node_priorities,
    uint32_t* __restrict__ node_indegrees,
    
    // Scheduling state
    uint32_t* __restrict__ current_schedule,
    uint32_t* __restrict__ schedule_len,
    uint32_t* __restrict__ last_scheduled_mask,
    
    // Output
    MergedAlgorithmState* __restrict__ state
) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t n = *n_agents;
    
    if (tid == 0) {
        state->iteration++;
        
        // Shared memory for reduction within this block
        __shared__ uint32_t s_active_pairs[1024];
        __shared__ uint32_t s_pair_count;
        
        if (threadIdx.x == 0) {
            s_pair_count = 0;
        }
        __syncthreads();
        
        // Each thread scans for active pairs
        for (uint32_t i = tid; i < n; i += blockDim.x * gridDim.x) {
            uint64_t w = net[i];
            if (WORD_DEAD(w)) continue;
            
            uint32_t nb_id = WORD_PORT_ADDR(w, 0);
            uint8_t nb_p = WORD_PORT_ID(w, 0);
            
            // Active pair: neighbor's principal points back to me
            if (nb_p == 0 && nb_id > i) {
                uint64_t nb_w = net[nb_id];
                if (!WORD_DEAD(nb_w)) {
                    uint32_t back_id = WORD_PORT_ADDR(nb_w, 0);
                    if (back_id == i) {
                        uint32_t slot = atomicAdd(&s_pair_count, 1);
                        if (slot < 1024) {
                            s_active_pairs[slot] = (i << 16) | nb_id;
                        }
                    }
                }
            }
        }
        
        __syncthreads();
        
        // Apply rules for active pairs (simplified - only apply first few)
        uint32_t pairs_to_apply = min(s_pair_count, 256u);
        for (uint32_t p = 0; p < pairs_to_apply; p++) {
            uint32_t pair = s_active_pairs[p];
            uint32_t a = pair >> 16;
            uint32_t b = pair & 0xFFFF;
            
            // Mark as dead and increment interaction count
            atomicOr((unsigned long long*)(net + a), 0x1ULL);
            atomicOr((unsigned long long*)(net + b), 0x1ULL);
            atomicAdd(&state->total_interactions, 1);
        }
        
        // Update agent count (simplified - just track deaths)
        // Full garbage collection would require prefix sum
    }
}

// =============================================================================
// Kernel: Dynamic Re-scheduling After Reduction
// 
// After agents are reduced/eliminated, update the conflict graph
// and re-run scheduling on the remaining active nodes.
// =============================================================================
__global__ void k_update_schedule_after_reduction(
    const uint64_t* __restrict__ net,
    const uint32_t n_agents,
    const uint64_t mr, const uint64_t mw, const uint64_t mk,
    const uint32_t theta_flow, const uint32_t theta_anti, const uint32_t theta_out,
    
    ConflictEdge* __restrict__ edges,
    uint32_t* __restrict__ edge_count,
    uint32_t* __restrict__ priorities,
    uint32_t* __restrict__ indegrees,
    
    uint32_t* __restrict__ ready_nodes,
    uint32_t* __restrict__ ready_count,
    PBitNodeState* __restrict__ pbit_states
) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    
    // Reset edge count
    if (tid == 0) {
        *edge_count = 0;
        *ready_count = 0;
    }
    __syncthreads();
    
    // Only process live agents
    if (tid < n_agents) {
        uint64_t w = net[tid];
        if (WORD_DEAD(w)) {
            priorities[tid] = 0;
            indegrees[tid] = 0xFFFFFFFF;  // Mark as dead
            return;
        }
        
        // Compute priority for this agent
        priorities[tid] = __popcll(w & mw);
        indegrees[tid] = 0;  // Will be updated during conflict detection
    }
    
    __syncthreads();
    
    // Detect conflicts among remaining agents (simplified)
    // In full implementation, this would use k_detect_conflicts
}

// =============================================================================
// Host Function: Run Merged GPU-STRAND-ICOR Algorithm
// =============================================================================

void run_merged_strand_icor_gpu(
    uint64_t* d_net,
    uint32_t initial_n_agents,
    uint32_t* d_alloc_head,
    
    // STRAND masks and thresholds
    uint64_t mr, uint64_t mw, uint64_t mk,
    uint32_t theta_flow, uint32_t theta_anti, uint32_t theta_out,
    
    // Output
    uint32_t* out_final_agents,
    uint32_t* out_total_interactions,
    uint32_t* out_schedule_cost,
    uint32_t* out_converged_iterations
);

// =============================================================================
// Optimized Pipeline: GPU-ICOR → STRAND (batched)
// 
// Run ICOR reduction to fixed point, then run STRAND scheduling once.
// This is the current best practice: minimize first, then schedule.
// =============================================================================
typedef struct {
    uint32_t initial_agents;
    uint32_t final_agents;
    uint32_t icor_iterations;
    uint32_t strand_schedule_cost;
    float icor_time_ms;
    float strand_time_ms;
    uint32_t* schedule;
} GpuStrandIcorResult;

void run_gpu_icor_then_strand(
    uint64_t* d_net,
    uint32_t n_agents,
    uint32_t* d_alloc_head,
    uint64_t mr, uint64_t mw, uint64_t mk,
    uint32_t theta_flow, uint32_t theta_anti, uint32_t theta_out,
    uint32_t max_icor_iterations,
    GpuStrandIcorResult* result
);

// =============================================================================
// Advanced: Incremental Scheduling During Reduction
// 
// Schedule agents as they become ready during reduction, maintaining
// optimal order throughout the process.
// =============================================================================
void run_incremental_strand_icor(
    uint64_t* d_net,
    uint32_t n_agents,
    uint32_t* d_alloc_head,
    uint64_t mr, uint64_t mw, uint64_t mk,
    uint32_t theta_flow, uint32_t theta_anti, uint32_t theta_out,
    GpuStrandIcorResult* result
);

#endif // STRAND_ICOR_MERGED_CUH
