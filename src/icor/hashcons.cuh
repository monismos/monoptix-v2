// icor/hashcons.cuh — Deterministic hashcons collapse using direct bit pattern comparison
// No hash table - derives conditions directly from the bit string structure

#ifndef ICOR_HASHCONS_CUH
#define ICOR_HASHCONS_CUH

#include "bits.cuh"
#include <cuda_runtime.h>
#include <cstdint>
#include <cstdlib>

// Normalize port references for comparison: replace agent indices with their canonical forms
// This is done iteratively until fixpoint
__global__ void k_normalize_ports(uint64_t *net, uint32_t *remap, uint32_t n) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n || WORD_DEAD(net[i])) return;

    uint64_t w = net[i];
    uint32_t p0 = WORD_PORTREF(w, 0);
    uint32_t p1 = WORD_PORTREF(w, 1);
    uint32_t p2 = WORD_PORTREF(w, 2);

    // Normalize each portref using remap table
    for (int k = 0; k < 3; k++) {
        uint32_t p = (k == 0) ? p0 : (k == 1) ? p1 : p2;
        uint32_t addr = p >> 2;
        uint32_t sel = p & 0x3;
        if (addr < n && addr != remap[addr]) {
            uint32_t new_p = (remap[addr] << 2) | sel;
            w = SET_PORT(w, k, new_p);
        }
    }

    net[i] = w;
}

// Compare two agent words for structural equality
// Two agents are equal if they have the same type and their portrefs point to equivalent agents
__device__ __forceinline__
bool agents_equal(uint64_t w1, uint64_t w2, uint32_t *remap, uint32_t n) {
    if (w1 == w2) return true;  // Fast path: identical bit patterns
    
    // Check type equality
    if (WORD_TYPE(w1) != WORD_TYPE(w2)) return false;
    
    // Check port equality after remapping
    for (int k = 0; k < 3; k++) {
        uint32_t p1 = WORD_PORTREF(w1, k);
        uint32_t p2 = WORD_PORTREF(w2, k);
        uint32_t a1 = p1 >> 2, s1 = p1 & 0x3;
        uint32_t a2 = p2 >> 2, s2 = p2 & 0x3;
        
        // Port selectors must match
        if (s1 != s2) return false;
        
        // After remapping, addresses should be equivalent
        uint32_t r1 = (a1 < n) ? remap[a1] : a1;
        uint32_t r2 = (a2 < n) ? remap[a2] : a2;
        if (r1 != r2) return false;
    }
    
    return true;
}

// Mark group starts in sorted agent array
__global__ void k_mark_agent_groups(uint64_t *net, uint32_t *sorted_idx, uint8_t *is_start, uint32_t n) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    
    if (i == 0) {
        is_start[0] = 1;
        return;
    }
    
    uint32_t curr = sorted_idx[i];
    uint32_t prev = sorted_idx[i-1];
    
    // Direct bit pattern comparison (no hashing)
    uint64_t w_curr = net[curr];
    uint64_t w_prev = net[prev];
    
    // Agents are in the same group if their bit patterns are identical
    is_start[i] = (w_curr != w_prev) ? 1 : 0;
}

// Assign canonical remap: each agent remaps to the first agent in its group
__global__ void k_assign_canonical_simple(uint32_t *sorted_idx, uint32_t *group_start, uint32_t *remap, uint32_t n) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    
    uint32_t canon_pos = group_start[i];
    uint32_t canon_orig = sorted_idx[canon_pos];
    remap[sorted_idx[i]] = canon_orig;
}

// Count how many agents changed their remap
__global__ void k_count_remap_changes(uint32_t *remap, uint32_t *old_remap, uint32_t *count, uint32_t n) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    if (remap[i] != old_remap[i]) atomicAdd(count, 1u);
}

// Initialize remap to identity
__global__ void k_init_remap_identity(uint32_t *remap, uint32_t n) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) remap[i] = i;
}

// Kill non-canonical duplicates
__global__ void k_kill_duplicates_simple(uint64_t *net, uint32_t *remap, uint32_t n) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n || WORD_DEAD(net[i])) return;
    if (remap[i] != i) net[i] = SET_DEAD(net[i]);
}

// Initialize sequence of indices
__global__ void k_init_sequence(uint32_t *idx, uint32_t n) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) idx[i] = i;
}

// GPU-side compaction (from compact.cuh)
extern uint32_t compact_bit_array_gpu(uint64_t* d_net, uint32_t n_agents);

typedef struct {
    uint64_t word;
    uint32_t index;
} HashEntry;

static int compare_hash_entry(const void* lhs, const void* rhs) {
    const HashEntry* a = (const HashEntry*)lhs;
    const HashEntry* b = (const HashEntry*)rhs;
    if (a->word < b->word) return -1;
    if (a->word > b->word) return 1;
    if (a->index < b->index) return -1;
    if (a->index > b->index) return 1;
    return 0;
}

// Host: hashcons_collapse - direct bit pattern comparison without hashing
void hashcons_collapse(uint64_t *d_net, uint32_t *n_inout) {
    uint32_t n = *n_inout;
    if (n == 0) return;

    uint64_t* h_net = (uint64_t*)malloc(sizeof(uint64_t) * n);
    HashEntry* entries = (HashEntry*)malloc(sizeof(HashEntry) * n);
    if (!h_net || !entries) {
        free(h_net);
        free(entries);
        return;
    }

    cudaMemcpy(h_net, d_net, sizeof(uint64_t) * n, cudaMemcpyDeviceToHost);
    for (uint32_t i = 0; i < n; i++) {
        entries[i].word = h_net[i];
        entries[i].index = i;
    }

    qsort(entries, n, sizeof(HashEntry), compare_hash_entry);
    for (uint32_t i = 1; i < n; i++) {
        if (entries[i].word == entries[i - 1].word) {
            h_net[entries[i].index] = SET_DEAD(h_net[entries[i].index]);
        }
    }
    cudaMemcpy(d_net, h_net, sizeof(uint64_t) * n, cudaMemcpyHostToDevice);
    n = compact_bit_array_gpu(d_net, n);

    free(h_net);
    free(entries);
    *n_inout = n;
}

#endif // ICOR_HASHCONS_CUH
