/*
 * STRAND Scheduling Kernels for GPU
 * 
 * GPU-accelerated conflict detection and scheduling for STRAND v2
 * Converts the CPU-based STRAND algorithm to CUDA kernels
 */

#ifndef STRAND_SCHEDULING_CUH
#define STRAND_SCHEDULING_CUH

#include <cuda_runtime.h>
#include <stdint.h>
#include <float.h>

// STRAND conflict types
#define CONFLICT_RAW   0x01  // Read-After-Write
#define CONFLICT_WAR   0x02  // Write-After-Read  
#define CONFLICT_WAW   0x04  // Write-After-Write

// Conflict edge structure for GPU
struct ConflictEdge {
    uint32_t from;
    uint32_t to;
    uint32_t flow_from_to : 1;
    uint32_t flow_to_from : 1;
    uint32_t output_conflict : 1;
    uint32_t order_weight : 8;
    uint32_t conflict_type : 5;
};

// Scheduling graph structure
struct SchedulingGraph {
    uint32_t* adjacency;      // Flattened adjacency list
    uint32_t* adj_offsets;    // Offsets into adjacency
    uint32_t* predecessors;   // Flattened predecessor list  
    uint32_t* pred_offsets;   // Offsets into predecessors
    uint32_t* indegree;       // Indegree for each node
    uint32_t node_count;
};

// P-bit state for probabilistic bit refinement
typedef struct {
    float magnetization;
    float base_score;
    uint32_t priority;
    uint32_t release_count;
} PBitState;

// =============================================================================
// Kernel 1: Conflict Detection (O(n²) pairwise comparison)
// =============================================================================
__global__ void k_detect_conflicts(
    const uint64_t* __restrict__ words,
    const uint32_t n_words,
    const uint64_t mr,           // read mask
    const uint64_t mw,           // write mask
    const uint32_t theta_flow,
    const uint32_t theta_anti,
    const uint32_t theta_out,
    ConflictEdge* __restrict__ out_edges,
    uint32_t* __restrict__ out_edge_count,
    uint32_t* __restrict__ priorities
) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    uint32_t total_pairs = n_words * (n_words - 1) / 2;
    
    if (tid >= total_pairs) return;
    
    // Convert linear index to (i, j) pair where i < j
    uint32_t i = 0;
    uint32_t j = 0;
    uint32_t remaining = tid;
    
    for (uint32_t row = 0; row < n_words - 1; row++) {
        uint32_t pairs_in_row = n_words - row - 1;
        if (remaining < pairs_in_row) {
            i = row;
            j = row + 1 + remaining;
            break;
        }
        remaining -= pairs_in_row;
    }
    
    if (i >= n_words || j >= n_words || i >= j) return;
    
    // Apply masks
    uint64_t wi_masked = words[i] & mw;
    uint64_t wj_masked = words[j] & mw;
    uint64_t ri_masked = words[i] & mr;
    uint64_t rj_masked = words[j] & mr;
    
    // Check for conflicts
    uint32_t flow_ij = __popcll(wi_masked & rj_masked) > theta_flow;
    uint32_t flow_ji = __popcll(wj_masked & ri_masked) > theta_anti;
    uint32_t out_conflict = __popcll(wi_masked & wj_masked) > theta_out;
    
    if (!(flow_ij || flow_ji || out_conflict)) return;
    
    // Determine ordering (higher priority goes first)
    uint32_t pi = __popcll(wi_masked);
    uint32_t pj = __popcll(wj_masked);
    
    uint32_t from, to;
    if (pi > pj || (pi == pj && i < j)) {
        from = i;
        to = j;
    } else {
        from = j;
        to = i;
    }
    
    // Compute order weight (lowest bit position of conflict)
    uint64_t witness = (wi_masked & rj_masked) | (wj_masked & ri_masked) | (wi_masked & wj_masked);
    uint32_t order_weight = 128;
    if (witness != 0) {
        order_weight = __ffsll((long long)witness) - 1;
    }
    
    // Write conflict edge
    uint32_t slot = atomicAdd(out_edge_count, 1);
    ConflictEdge edge;
    edge.from = from;
    edge.to = to;
    edge.flow_from_to = flow_ij;
    edge.flow_to_from = flow_ji;
    edge.output_conflict = out_conflict;
    edge.order_weight = order_weight;
    
    // Classify conflict type
    if (flow_ij && !flow_ji && !out_conflict) edge.conflict_type = CONFLICT_RAW;
    else if (!flow_ij && flow_ji && !out_conflict) edge.conflict_type = CONFLICT_WAR;
    else if (out_conflict && !flow_ij && !flow_ji) edge.conflict_type = CONFLICT_WAW;
    else edge.conflict_type = CONFLICT_RAW | CONFLICT_WAR | CONFLICT_WAW;
    
    out_edges[slot] = edge;
    
    // Store priorities
    if (tid < n_words) {
        priorities[tid] = __popcll(words[tid] & mw);
    }
}

// =============================================================================
// Kernel 2: Build Scheduling Graph from Conflicts
// =============================================================================
__global__ void k_build_graph(
    const ConflictEdge* __restrict__ edges,
    const uint32_t edge_count,
    const uint32_t node_count,
    uint32_t* __restrict__ adjacency,
    uint32_t* __restrict__ adj_offsets,
    uint32_t* __restrict__ predecessors,
    uint32_t* __restrict__ pred_offsets,
    uint32_t* __restrict__ indegree
) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    
    if (tid == 0) {
        // Initialize offsets
        for (uint32_t i = 0; i <= node_count; i++) {
            adj_offsets[i] = 0;
            pred_offsets[i] = 0;
        }
        
        // Count edges per node
        for (uint32_t i = 0; i < edge_count; i++) {
            adj_offsets[edges[i].from + 1]++;
            pred_offsets[edges[i].to + 1]++;
        }
        
        // Prefix sum to get offsets
        for (uint32_t i = 1; i <= node_count; i++) {
            adj_offsets[i] += adj_offsets[i - 1];
            pred_offsets[i] += pred_offsets[i - 1];
        }
        
        // Initialize indegree
        for (uint32_t i = 0; i < node_count; i++) {
            indegree[i] = pred_offsets[i + 1] - pred_offsets[i];
        }
    }
}

__global__ void k_fill_graph(
    const ConflictEdge* __restrict__ edges,
    const uint32_t edge_count,
    const uint32_t node_count,
    uint32_t* __restrict__ adjacency,
    uint32_t* __restrict__ adj_offsets,
    uint32_t* __restrict__ predecessors,
    uint32_t* __restrict__ pred_offsets,
    uint32_t* __restrict__ adj_counters,
    uint32_t* __restrict__ pred_counters
) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= edge_count) return;
    
    ConflictEdge edge = edges[tid];
    
    // Add to adjacency list of 'from' node
    uint32_t adj_slot = atomicAdd(&adj_counters[edge.from], 1);
    adjacency[adj_offsets[edge.from] + adj_slot] = edge.to;
    
    // Add to predecessor list of 'to' node
    uint32_t pred_slot = atomicAdd(&pred_counters[edge.to], 1);
    predecessors[pred_offsets[edge.to] + pred_slot] = edge.from;
}

// =============================================================================
// Kernel 3: Dead Word Detection
// =============================================================================
__global__ void k_detect_dead_words(
    const uint64_t* __restrict__ words,
    const uint32_t n_words,
    const uint64_t mr,
    const uint64_t mw,
    const uint64_t mk,
    uint32_t* __restrict__ dead_flags
) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= n_words) return;
    
    uint64_t word = words[tid];
    uint32_t read_bits = __popcll(word & mr);
    uint32_t write_bits = __popcll(word & mw);
    uint32_t kill_bits = __popcll(word & mk);
    
    dead_flags[tid] = (read_bits == 0 && write_bits == 0 && kill_bits == 0) ? 1 : 0;
}

// =============================================================================
// Kernel 4: Mask Derivation (Entropy-based)
// =============================================================================
__global__ void k_compute_entropies(
    const uint64_t* __restrict__ words,
    const uint32_t n_words,
    float* __restrict__ entropies
) {
    uint32_t bit_pos = blockIdx.x * blockDim.x + threadIdx.x;
    if (bit_pos >= 64) return;
    
    uint64_t mask = 1ULL << bit_pos;
    uint32_t pop_count = 0;
    
    for (uint32_t i = 0; i < n_words; i++) {
        pop_count += (words[i] & mask) != 0;
    }
    
    float p = (float)pop_count / (float)n_words;
    if (p <= 0.0f || p >= 1.0f) {
        entropies[bit_pos] = 0.0f;
    } else {
        entropies[bit_pos] = -(p * log2f(p)) - ((1.0f - p) * log2f(1.0f - p));
    }
}

// Host function to derive masks from GPU
void derive_masks_gpu(
    const uint64_t* d_words,
    uint32_t n_words,
    uint64_t* out_mr,
    uint64_t* out_mw,
    uint64_t* out_mk
);

// Host function to build scheduling graph on GPU
void build_scheduling_graph_gpu(
    const uint64_t* d_words,
    uint32_t n_words,
    uint64_t mr, uint64_t mw,
    uint32_t theta_flow, uint32_t theta_anti, uint32_t theta_out,
    SchedulingGraph* out_graph
);

// Host function to detect dead words on GPU
uint32_t detect_dead_words_gpu(
    const uint64_t* d_words,
    uint32_t n_words,
    uint64_t mr, uint64_t mw, uint64_t mk,
    uint32_t** out_dead_indices
);

#endif // STRAND_SCHEDULING_CUH
