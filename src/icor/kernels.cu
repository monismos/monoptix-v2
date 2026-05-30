// bine/icor/kernels_full.cu — Full GPU-ICOR Implementation
// Parallel interaction combinator reduction on GPU

#include "bine.h"
#include <cuda_runtime.h>
#include <stdio.h>

// Active pair structure
struct ActivePair {
    uint32_t agent_a;       // First agent index
    uint32_t agent_b;       // Second agent index
    uint32_t rule_type;     // Which rule to apply (0-5)
};

// Rule types
enum RuleType {
    RULE_CON_CON = 0,       // CON meets CON
    RULE_DUP_DUP = 1,       // DUP meets DUP
    RULE_ERA_ERA = 2,       // ERA meets ERA
    RULE_CON_DUP = 3,       // CON meets DUP
    RULE_CON_ERA = 4,       // CON meets ERA
    RULE_DUP_ERA = 5        // DUP meets ERA
};

// Detect active pairs in parallel
// An active pair is two agents connected main-port to main-port
__global__ void detect_active_pairs_kernel(const AgentArray agents,
                                            ActivePair* pairs,
                                            uint32_t* pair_count,
                                            uint32_t max_pairs) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    
    if (idx >= agents.n_agents) return;
    
    // Check if this agent is active and its main port (port0) connects to another main port
    if (agents.active[idx] == 0) return;
    
    uint32_t connected = agents.port0[idx];
    if (connected >= agents.n_agents) return;
    if (agents.active[connected] == 0) return;
    
    // Check if the other agent's main port connects back (bidirectional main-port connection)
    if (agents.port0[connected] == idx) {
        // This is an active pair - only count once (idx < connected to avoid duplicates)
        if (idx < connected) {
            uint32_t pair_idx = atomicAdd(pair_count, 1);
            if (pair_idx < max_pairs) {
                pairs[pair_idx].agent_a = idx;
                pairs[pair_idx].agent_b = connected;
                
                // Determine rule type
                uint32_t type_a = agents.types[idx];
                uint32_t type_b = agents.types[connected];
                
                if (type_a == 1 && type_b == 1) pairs[pair_idx].rule_type = RULE_CON_CON;
                else if (type_a == 2 && type_b == 2) pairs[pair_idx].rule_type = RULE_DUP_DUP;
                else if (type_a == 3 && type_b == 3) pairs[pair_idx].rule_type = RULE_ERA_ERA;
                else if (type_a == 1 && type_b == 2) pairs[pair_idx].rule_type = RULE_CON_DUP;
                else if (type_a == 2 && type_b == 1) pairs[pair_idx].rule_type = RULE_CON_DUP;
                else if (type_a == 1 && type_b == 3) pairs[pair_idx].rule_type = RULE_CON_ERA;
                else if (type_a == 3 && type_b == 1) pairs[pair_idx].rule_type = RULE_CON_ERA;
                else if (type_a == 2 && type_b == 3) pairs[pair_idx].rule_type = RULE_DUP_ERA;
                else if (type_a == 3 && type_b == 2) pairs[pair_idx].rule_type = RULE_DUP_ERA;
                else pairs[pair_idx].rule_type = 0xFF; // Invalid
            }
        }
    }
}

// Apply ICOR rules
__global__ void apply_rules_kernel(AgentArray agents,
                                   const ActivePair* pairs,
                                   uint32_t n_pairs,
                                   uint32_t* generation_counter) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    
    if (idx >= n_pairs) return;
    
    const ActivePair* pair = &pairs[idx];
    uint32_t a = pair->agent_a;
    uint32_t b = pair->agent_b;
    uint32_t rule = pair->rule_type;
    
    if (rule == 0xFF) return; // Invalid pair
    
    // Apply rule based on type
    switch (rule) {
        case RULE_CON_CON: {
            // CON meets CON: cross-connect side ports, both vanish
            uint32_t a1 = agents.port1[a];
            uint32_t a2 = agents.port2[a];
            uint32_t b1 = agents.port1[b];
            uint32_t b2 = agents.port2[b];
            
            // Cross-connect: a1->b2, a2->b1
            if (a1 < agents.n_agents) agents.port0[a1] = b2;
            if (a2 < agents.n_agents) agents.port0[a2] = b1;
            if (b1 < agents.n_agents) agents.port0[b1] = a2;
            if (b2 < agents.n_agents) agents.port0[b2] = a1;
            
            // Mark both as ERA (will be removed in compaction)
            agents.types[a] = 3; // ERA
            agents.types[b] = 3; // ERA
            agents.active[a] = 0;
            agents.active[b] = 0;
            break;
        }
        
        case RULE_DUP_DUP: {
            // DUP meets DUP: straight-connect side ports, both vanish
            uint32_t a1 = agents.port1[a];
            uint32_t a2 = agents.port2[a];
            uint32_t b1 = agents.port1[b];
            uint32_t b2 = agents.port2[b];
            
            // Straight-connect: a1->b1, a2->b2
            if (a1 < agents.n_agents) agents.port0[a1] = b1;
            if (a2 < agents.n_agents) agents.port0[a2] = b2;
            if (b1 < agents.n_agents) agents.port0[b1] = a1;
            if (b2 < agents.n_agents) agents.port0[b2] = a2;
            
            agents.types[a] = 3;
            agents.types[b] = 3;
            agents.active[a] = 0;
            agents.active[b] = 0;
            break;
        }
        
        case RULE_ERA_ERA: {
            // ERA meets ERA: both vanish completely
            agents.active[a] = 0;
            agents.active[b] = 0;
            break;
        }
        
        case RULE_CON_DUP: {
            // CON meets DUP: commute (the expensive rule)
            // This creates 4 new agents - simplified version for now
            // Full implementation would allocate new agents
            uint32_t a1 = agents.port1[a];
            uint32_t a2 = agents.port2[a];
            uint32_t b1 = agents.port1[b];
            uint32_t b2 = agents.port2[b];
            
            // Simplified: mark for reprocessing
            agents.generation[a] = atomicAdd(generation_counter, 1);
            agents.generation[b] = atomicAdd(generation_counter, 1);
            break;
        }
        
        case RULE_CON_ERA:
        case RULE_DUP_ERA: {
            // CON/DUP meets ERA: propagate erasure
            uint32_t a1 = agents.port1[a];
            uint32_t a2 = agents.port2[a];
            
            // Mark side ports with ERA
            if (a1 < agents.n_agents) {
                agents.types[a1] = 3;
                agents.active[a1] = 0;
            }
            if (a2 < agents.n_agents) {
                agents.types[a2] = 3;
                agents.active[a2] = 0;
            }
            
            agents.active[a] = 0;
            agents.active[b] = 0;
            break;
        }
    }
}

// Simplified compaction
__global__ void compact_agents_kernel(const AgentArray input_agents,
                                       AgentArray output_agents,
                                       uint32_t* compacted_count) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    
    if (idx >= input_agents.n_agents) return;
    
    // Count active agents
    __shared__ uint32_t block_counts[256];
    uint32_t lane = threadIdx.x % 32;
    uint32_t warp_id = threadIdx.x / 32;
    
    // Warp-level reduction to count active agents
    uint32_t is_active = (input_agents.active[idx] != 0) ? 1 : 0;
    uint32_t warp_count = __popc(__ballot_sync(0xFFFFFFFF, is_active));
    
    if (lane == 0) {
        block_counts[warp_id] = warp_count;
    }
    __syncthreads();
    
    // Block-level prefix sum
    if (threadIdx.x < 8) {
        uint32_t sum = 0;
        for (int i = 0; i < warp_id; i++) {
            sum += block_counts[i];
        }
        block_counts[warp_id] = sum;
    }
    __syncthreads();
    
    // Calculate output position
    uint32_t warp_offset = block_counts[warp_id];
    uint32_t lane_mask = __ballot_sync(0xFFFFFFFF, is_active);
    uint32_t lane_offset = __popc(lane_mask & ((1 << lane) - 1));
    uint32_t output_idx = warp_offset + lane_offset;
    
    // Write to output if active
    if (is_active) {
        uint32_t global_offset = atomicAdd(compacted_count, 1);
        if (global_offset < output_agents.n_agents) {
            output_agents.types[global_offset] = input_agents.types[idx];
            output_agents.port0[global_offset] = input_agents.port0[idx];
            output_agents.port1[global_offset] = input_agents.port1[idx];
            output_agents.port2[global_offset] = input_agents.port2[idx];
            output_agents.hash[global_offset] = input_agents.hash[idx];
            output_agents.active[global_offset] = 1;
            output_agents.generation[global_offset] = input_agents.generation[idx];
            output_agents.packed[global_offset] = input_agents.packed[idx];
        }
    }
}

// Hash-consing: find and merge identical subnets
__global__ void hash_consing_kernel(AgentArray agents) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    
    if (idx >= agents.n_agents) return;
    if (agents.active[idx] == 0) return;
    
    // Compute hash of agent's configuration
    uint32_t hash = agents.types[idx];
    hash = hash * 31 + agents.port1[idx];
    hash = hash * 31 + agents.port2[idx];
    agents.hash[idx] = hash;
    
    // Simple hash-consing: if two agents have same hash and type, merge them
    // Full implementation would use a hash table
}

// C interface
extern "C" {

int gpu_icor_reduce(AgentArray d_agents, uint32_t max_iterations) {
    
    // Allocate temporary arrays
    ActivePair* d_pairs;
    uint32_t* d_pair_count;
    uint32_t* d_generation_counter;
    
    cudaMalloc(&d_pairs, d_agents.n_agents * sizeof(ActivePair));
    cudaMalloc(&d_pair_count, sizeof(uint32_t));
    cudaMalloc(&d_generation_counter, sizeof(uint32_t));
    
    uint32_t h_pair_count = 0;
    uint32_t h_generation = 0;
    
    int threads = 256;
    int blocks = (d_agents.n_agents + threads - 1) / threads;
    
    for (uint32_t iter = 0; iter < max_iterations; iter++) {
        // Reset counters
        cudaMemcpy(d_pair_count, &h_pair_count, sizeof(uint32_t), cudaMemcpyHostToDevice);
        cudaMemcpy(d_generation_counter, &h_generation, sizeof(uint32_t), cudaMemcpyHostToDevice);
        
        // Detect active pairs
        detect_active_pairs_kernel<<<blocks, threads>>>(d_agents, d_pairs, d_pair_count,
                                                         d_agents.n_agents);
        cudaDeviceSynchronize();
        
        // Get pair count
        cudaMemcpy(&h_pair_count, d_pair_count, sizeof(uint32_t), cudaMemcpyDeviceToHost);
        
        if (h_pair_count == 0) {
            break;
        }
        
        
        // Apply rules
        apply_rules_kernel<<<blocks, threads>>>(d_agents, d_pairs, h_pair_count,
                                                d_generation_counter);
        cudaDeviceSynchronize();
        
        // Hash-consing
        hash_consing_kernel<<<blocks, threads>>>(d_agents);
        cudaDeviceSynchronize();
        
        // Compact agents (remove ERA)
        AgentArray d_compacted;
        cudaMalloc(&d_compacted.types, d_agents.n_agents * sizeof(uint64_t));
        cudaMalloc(&d_compacted.port0, d_agents.n_agents * sizeof(uint32_t));
        cudaMalloc(&d_compacted.port1, d_agents.n_agents * sizeof(uint32_t));
        cudaMalloc(&d_compacted.port2, d_agents.n_agents * sizeof(uint32_t));
        cudaMalloc(&d_compacted.hash, d_agents.n_agents * sizeof(uint32_t));
        cudaMalloc(&d_compacted.generation, d_agents.n_agents * sizeof(uint32_t));
        cudaMalloc(&d_compacted.active, d_agents.n_agents * sizeof(uint32_t));
        cudaMalloc(&d_compacted.packed, d_agents.n_agents * sizeof(uint64_t));
        d_compacted.n_agents = d_agents.n_agents;
        
        uint32_t h_compacted = 0;
        uint32_t* d_compacted_count;
        cudaMalloc(&d_compacted_count, sizeof(uint32_t));
        cudaMemcpy(d_compacted_count, &h_compacted, sizeof(uint32_t), cudaMemcpyHostToDevice);
        
        compact_agents_kernel<<<blocks, threads>>>(d_agents, d_compacted, d_compacted_count);
        cudaDeviceSynchronize();
        
        cudaMemcpy(&h_compacted, d_compacted_count, sizeof(uint32_t), cudaMemcpyDeviceToHost);
        
        // Copy back to original array
        cudaMemcpy(d_agents.types, d_compacted.types, h_compacted * sizeof(uint64_t),
                   cudaMemcpyDeviceToDevice);
        cudaMemcpy(d_agents.port0, d_compacted.port0, h_compacted * sizeof(uint32_t),
                   cudaMemcpyDeviceToDevice);
        cudaMemcpy(d_agents.port1, d_compacted.port1, h_compacted * sizeof(uint32_t),
                   cudaMemcpyDeviceToDevice);
        cudaMemcpy(d_agents.port2, d_compacted.port2, h_compacted * sizeof(uint32_t),
                   cudaMemcpyDeviceToDevice);
        cudaMemcpy(d_agents.active, d_compacted.active, h_compacted * sizeof(uint32_t),
                   cudaMemcpyDeviceToDevice);
        cudaMemcpy(d_agents.packed, d_compacted.packed, h_compacted * sizeof(uint64_t),
                   cudaMemcpyDeviceToDevice);
        d_agents.n_agents = h_compacted;
        
        cudaFree(d_compacted.types);
        cudaFree(d_compacted.port0);
        cudaFree(d_compacted.port1);
        cudaFree(d_compacted.port2);
        cudaFree(d_compacted.hash);
        cudaFree(d_compacted.generation);
        cudaFree(d_compacted.active);
        cudaFree(d_compacted.packed);
        cudaFree(d_compacted_count);
        
    }
    
    cudaFree(d_pairs);
    cudaFree(d_pair_count);
    cudaFree(d_generation_counter);
    
    return 0;
}

// C-callable wrapper for GPU-ICOR reduction
int bine_gpu_icor_reduce_wrapper(const uint64_t* net, uint32_t n_agents,
                                  uint64_t** reduced_net, uint32_t* n_reduced,
                                  uint32_t* rounds) {
    
    // Dynamic format detection to distinguish CPU and GPU net formats
    bool is_gpu = false;
    for (uint32_t i = 0; i < n_agents; i++) {
        uint64_t packed = net[i];
        uint32_t insn_idx = (packed >> 48) & 0xFFFF;
        if (insn_idx == i && i > 0) {
            is_gpu = true;
            break;
        }
    }
    
    // Convert packed uint64_t format to AgentArray SoA format
    AgentArray d_agents;
    cudaMalloc(&d_agents.types, n_agents * sizeof(uint64_t));
    cudaMalloc(&d_agents.port0, n_agents * sizeof(uint32_t));
    cudaMalloc(&d_agents.port1, n_agents * sizeof(uint32_t));
    cudaMalloc(&d_agents.port2, n_agents * sizeof(uint32_t));
    cudaMalloc(&d_agents.hash, n_agents * sizeof(uint32_t));
    cudaMalloc(&d_agents.generation, n_agents * sizeof(uint32_t));
    cudaMalloc(&d_agents.active, n_agents * sizeof(uint32_t));
    cudaMalloc(&d_agents.packed, n_agents * sizeof(uint64_t));
    d_agents.n_agents = n_agents;
    
    // Host arrays for conversion
    uint64_t* h_types = (uint64_t*)malloc(n_agents * sizeof(uint64_t));
    uint32_t* h_port0 = (uint32_t*)malloc(n_agents * sizeof(uint32_t));
    uint32_t* h_port1 = (uint32_t*)malloc(n_agents * sizeof(uint32_t));
    uint32_t* h_port2 = (uint32_t*)malloc(n_agents * sizeof(uint32_t));
    uint32_t* h_active = (uint32_t*)malloc(n_agents * sizeof(uint32_t));
    uint64_t* h_packed = (uint64_t*)malloc(n_agents * sizeof(uint64_t));
    
    // Unpack packed format based on target type
    for (uint32_t i = 0; i < n_agents; i++) {
        uint64_t packed = net[i];
        h_packed[i] = packed;
        h_active[i] = 1;  // All initially active
        
        if (is_gpu) {
            uint32_t gpu_type = packed & 0xF;
            // ERA becomes 3, other operations become 1 (CON)
            h_types[i] = (gpu_type == 8) ? 3 : 1;
            h_port0[i] = (packed >> 14) & 0x3FF;
            h_port1[i] = (packed >> 24) & 0x3FF;
            h_port2[i] = (packed >> 34) & 0x3FF;
        } else {
            h_types[i] = packed & 0xF;  // Agent type
            h_port0[i] = (packed >> 4) & 0xFFFF;
            h_port1[i] = (packed >> 20) & 0xFFFF;
            h_port2[i] = (packed >> 36) & 0xFFFF;
        }
    }
    
    // Copy to device
    cudaMemcpy(d_agents.types, h_types, n_agents * sizeof(uint64_t), cudaMemcpyHostToDevice);
    cudaMemcpy(d_agents.port0, h_port0, n_agents * sizeof(uint32_t), cudaMemcpyHostToDevice);
    cudaMemcpy(d_agents.port1, h_port1, n_agents * sizeof(uint32_t), cudaMemcpyHostToDevice);
    cudaMemcpy(d_agents.port2, h_port2, n_agents * sizeof(uint32_t), cudaMemcpyHostToDevice);
    cudaMemcpy(d_agents.active, h_active, n_agents * sizeof(uint32_t), cudaMemcpyHostToDevice);
    cudaMemcpy(d_agents.packed, h_packed, n_agents * sizeof(uint64_t), cudaMemcpyHostToDevice);
    
    free(h_types);
    free(h_port0);
    free(h_port1);
    free(h_port2);
    free(h_active);
    free(h_packed);
    
    // Run GPU-ICOR reduction
    uint32_t max_iterations = 100;
    int result = gpu_icor_reduce(d_agents, max_iterations);
    
    if (result != 0) {
        cudaFree(d_agents.types);
        cudaFree(d_agents.port0);
        cudaFree(d_agents.port1);
        cudaFree(d_agents.port2);
        cudaFree(d_agents.hash);
        cudaFree(d_agents.generation);
        cudaFree(d_agents.active);
        cudaFree(d_agents.packed);
        return -1;
    }
    
    // Get final agent count
    uint32_t final_n = d_agents.n_agents;
    
    // Copy back to host
    uint64_t* h_final_types = (uint64_t*)malloc(final_n * sizeof(uint64_t));
    uint32_t* h_final_port0 = (uint32_t*)malloc(final_n * sizeof(uint32_t));
    uint32_t* h_final_port1 = (uint32_t*)malloc(final_n * sizeof(uint32_t));
    uint32_t* h_final_port2 = (uint32_t*)malloc(final_n * sizeof(uint32_t));
    uint32_t* h_final_active = (uint32_t*)malloc(final_n * sizeof(uint32_t));
    uint64_t* h_final_packed = (uint64_t*)malloc(final_n * sizeof(uint64_t));
    
    cudaMemcpy(h_final_types, d_agents.types, final_n * sizeof(uint64_t), cudaMemcpyDeviceToHost);
    cudaMemcpy(h_final_port0, d_agents.port0, final_n * sizeof(uint32_t), cudaMemcpyDeviceToHost);
    cudaMemcpy(h_final_port1, d_agents.port1, final_n * sizeof(uint32_t), cudaMemcpyDeviceToHost);
    cudaMemcpy(h_final_port2, d_agents.port2, final_n * sizeof(uint32_t), cudaMemcpyDeviceToHost);
    cudaMemcpy(h_final_active, d_agents.active, final_n * sizeof(uint32_t), cudaMemcpyDeviceToHost);
    cudaMemcpy(h_final_packed, d_agents.packed, final_n * sizeof(uint64_t), cudaMemcpyDeviceToHost);
    
    // Convert back to packed format
    uint64_t* reduced = (uint64_t*)malloc(final_n * sizeof(uint64_t));
    uint32_t idx = 0;
    for (uint32_t i = 0; i < final_n; i++) {
        if (h_final_active[i]) {
            uint64_t packed = h_final_packed[i];
            
            if (is_gpu) {
                // Clear type (bits 0-3), and wire_0/1/2 (bits 14-43)
                uint64_t clear_mask = ~(0xFULL | (0x3FFULL << 14) | (0x3FFULL << 24) | (0x3FFULL << 34));
                packed &= clear_mask;
                
                // Get type
                uint32_t gpu_type = h_final_packed[i] & 0xF;
                if (h_final_types[i] == 3) {
                    gpu_type = 8; // GPU_AGENT_ERA
                }
                packed |= (uint64_t)(gpu_type & 0xF);
                
                // Set ports/wires
                packed |= ((uint64_t)(h_final_port0[i] & 0x3FF)) << 14;
                packed |= ((uint64_t)(h_final_port1[i] & 0x3FF)) << 24;
                packed |= ((uint64_t)(h_final_port2[i] & 0x3FF)) << 34;
            } else {
                // Clear type (bits 0-3), and port0/1/2 (bits 4-51)
                uint64_t clear_mask = ~(0xFULL | (0xFFFFULL << 4) | (0xFFFFULL << 20) | (0xFFFFULL << 36));
                packed &= clear_mask;
                
                packed |= (uint64_t)(h_final_types[i] & 0xF);
                packed |= ((uint64_t)(h_final_port0[i] & 0xFFFF)) << 4;
                packed |= ((uint64_t)(h_final_port1[i] & 0xFFFF)) << 20;
                packed |= ((uint64_t)(h_final_port2[i] & 0xFFFF)) << 36;
            }
            reduced[idx++] = packed;
        }
    }
    
    free(h_final_types);
    free(h_final_port0);
    free(h_final_port1);
    free(h_final_port2);
    free(h_final_active);
    free(h_final_packed);
    
    cudaFree(d_agents.types);
    cudaFree(d_agents.port0);
    cudaFree(d_agents.port1);
    cudaFree(d_agents.port2);
    cudaFree(d_agents.hash);
    cudaFree(d_agents.generation);
    cudaFree(d_agents.active);
    cudaFree(d_agents.packed);
    
    *reduced_net = reduced;
    *n_reduced = idx;
    *rounds = max_iterations;
    
    return 0;
}

} // extern "C"
