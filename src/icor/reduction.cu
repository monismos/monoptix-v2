// bine/icor/kernels.cu — Stage 4: GPU-ICOR Reduction Kernels
// Implements the 6 interaction rules on GPU for parallel reduction.

#include "bine.h"
#include <cuda_runtime.h>
#include <device_launch_parameters.h>
#include <stdio.h>

// ICOR rule types
#define ICOR_CON_CON   0
#define ICOR_DUP_DUP   1
#define ICOR_ERA_ERA   2
#define ICOR_CON_DUP   3
#define ICOR_ERA_CON   4
#define ICOR_ERA_DUP   5

// Check if two agents can interact via a specific rule
__device__ bool can_interact(uint64_t a, uint64_t b, int rule_type) {
    AgentPacked ap = *(AgentPacked*)&a;
    AgentPacked bp = *(AgentPacked*)&b;
    
    switch (rule_type) {
        case ICOR_CON_CON:
            // CON >< CON: annihilate if connected
            return (ap.type == 0 && bp.type == 0) && 
                   (ap.port_0 == bp.port_1 || ap.port_1 == bp.port_0);
        
        case ICOR_DUP_DUP:
            // DUP >< DUP: annihilate if sharing same value
            return (ap.type == 1 && bp.type == 1) &&
                   (ap.port_0 == bp.port_0);
        
        case ICOR_ERA_ERA:
            // ERA >< ERA: annihilate
            return (ap.type == 2 && bp.type == 2);
        
        case ICOR_CON_DUP:
            // CON >< DUP: commutation
            return (ap.type == 0 && bp.type == 1) ||
                   (ap.type == 1 && bp.type == 0);
        
        case ICOR_ERA_CON:
            // ERA >< CON: dead code elimination
            return (ap.type == 2 && bp.type == 0) ||
                   (ap.type == 0 && bp.type == 2);
        
        case ICOR_ERA_DUP:
            // ERA >< DUP: dead code elimination
            return (ap.type == 2 && bp.type == 1) ||
                   (ap.type == 1 && bp.type == 2);
        
        default:
            return false;
    }
}

// Apply ICOR rule to a pair of agents
__global__ void k_apply_rules(uint64_t* net, uint2* active_pairs,
                             uint32_t n_active, int rule_type) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    
    if (tid >= n_active) return;
    
    uint2 pair = active_pairs[tid];
    uint64_t a = net[pair.x];
    uint64_t b = net[pair.y];
    
    AgentPacked ap = *(AgentPacked*)&a;
    AgentPacked bp = *(AgentPacked*)&b;
    
    // Do not eliminate essential agents
    bool a_essential = ap.essential;
    bool b_essential = bp.essential;
    
    switch (rule_type) {
        case ICOR_CON_CON:
            // Commutation: swap if no data dependency
            // Do not annihilate essential agents
            if (ap.type == 0 && bp.type == 0) {
                if (!a_essential && !b_essential) {
                    // Both non-essential - can annihilate
                    ap.type = 2; // ERA
                    bp.type = 2; // ERA
                } else {
                    // At least one is essential - just swap ports
                    uint32_t tmp = ap.port_0;
                    ap.port_0 = bp.port_0;
                    bp.port_0 = tmp;
                }
            }
            break;
        
        case ICOR_DUP_DUP:
            // Annihilation: both become ERA (unless essential)
            if (!a_essential) ap.type = 2;
            if (!b_essential) bp.type = 2;
            break;
        
        case ICOR_ERA_ERA:
            // Both vanish (already ERA)
            break;
        
        case ICOR_CON_DUP:
            // Commutation: create k copies
            if (ap.type == 0 && bp.type == 1) {
                // CON followed by DUP → distribute
                if (!b_essential) bp.type = 0; // Become CON
            } else if (ap.type == 1 && bp.type == 0) {
                if (!a_essential) ap.type = 0; // Become CON
            }
            break;
        
        case ICOR_ERA_CON:
            // Dead code elimination: CON becomes ERA, propagate ERA to inputs
            // Skip if CON is essential
            if (ap.type == 2 && bp.type == 0 && !b_essential) {
                bp.type = 2; // CON becomes ERA
            } else if (ap.type == 0 && bp.type == 2 && !a_essential) {
                ap.type = 2; // CON becomes ERA
            }
            break;
        
        case ICOR_ERA_DUP:
            // Dead code elimination: DUP becomes ERA (unless essential)
            if (ap.type == 2 && bp.type == 1 && !b_essential) {
                bp.type = 2; // DUP becomes ERA
            } else if (ap.type == 1 && bp.type == 2 && !a_essential) {
                ap.type = 2; // DUP becomes ERA
            }
            break;
    }
    
    net[pair.x] = *(uint64_t*)&ap;
    net[pair.y] = *(uint64_t*)&bp;
}

// GPU kernel: find active pairs for a specific rule
__global__ void k_find_active_pairs(uint64_t* net, uint32_t n_agents,
                                     int rule_type, uint2* active_pairs,
                                     uint32_t* n_active) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    
    if (tid >= n_agents * n_agents) return;
    
    uint32_t i = tid / n_agents;
    uint32_t j = tid % n_agents;
    
    if (i >= j) return; // Avoid duplicates and self-interaction
    
    if (can_interact(net[i], net[j], rule_type)) {
        uint32_t idx = atomicAdd(n_active, 1);
        if (idx < n_agents * n_agents) {
            active_pairs[idx] = make_uint2(i, j);
        }
    }
}

// GPU kernel: count agents by type
__global__ void k_count_agents(uint64_t* net, uint32_t n_agents,
                               uint32_t* counts) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    
    if (tid >= n_agents) return;
    
    AgentPacked ap = *(AgentPacked*)&net[tid];
    atomicAdd(&counts[ap.type], 1);
}

// Main GPU-ICOR reduction function
uint64_t* bine_icor_reduce(const uint64_t* net, uint32_t n_agents,
                          uint32_t* n_reduced, uint32_t* rounds) {
    if (!net || n_agents == 0) {
        *n_reduced = 0;
        *rounds = 0;
        return NULL;
    }
    
    // Allocate device memory
    uint64_t* d_net;
    cudaMalloc(&d_net, n_agents * sizeof(uint64_t));
    cudaMemcpy(d_net, net, n_agents * sizeof(uint64_t), cudaMemcpyHostToDevice);
    
    uint2* d_active_pairs;
    cudaMalloc(&d_active_pairs, n_agents * n_agents * sizeof(uint2));
    uint32_t* d_n_active;
    cudaMalloc(&d_n_active, sizeof(uint32_t));
    
    uint32_t* d_counts;
    cudaMalloc(&d_counts, 5 * sizeof(uint32_t)); // 5 agent types
    
    uint32_t max_rounds = 100;
    uint32_t round = 0;
    uint32_t prev_active = UINT32_MAX;
    
    for (round = 0; round < max_rounds; round++) {
        uint32_t total_active = 0;
        
        // Try each rule type
        for (int rule = 0; rule < 6; rule++) {
            cudaMemset(d_n_active, 0, sizeof(uint32_t));
            
            // Find active pairs for this rule
            uint32_t threads = 256;
            uint32_t blocks = (n_agents * n_agents + threads - 1) / threads;
            k_find_active_pairs<<<blocks, threads>>>(d_net, n_agents, rule,
                                                    d_active_pairs, d_n_active);
            
            uint32_t n_active = 0;
            cudaMemcpy(&n_active, d_n_active, sizeof(uint32_t), cudaMemcpyDeviceToHost);
            
            if (n_active > 0) {
                // Apply rules to active pairs
                blocks = (n_active + threads - 1) / threads;
                k_apply_rules<<<blocks, threads>>>(d_net, d_active_pairs, n_active, rule);
                
                total_active += n_active;
            }
        }
        
        // Check for convergence
        if (total_active == 0 || total_active >= prev_active) {
            break;
        }
        prev_active = total_active;
    }
    
    // Copy result back
    uint64_t* result_net = (uint64_t*)malloc(n_agents * sizeof(uint64_t));
    cudaMemcpy(result_net, d_net, n_agents * sizeof(uint64_t), cudaMemcpyDeviceToHost);
    
    // Count remaining agents by type
    uint32_t h_counts[5] = {0};
    cudaMemcpy(d_counts, h_counts, 5 * sizeof(uint32_t), cudaMemcpyHostToDevice);
    cudaMemset(d_counts, 0, 5 * sizeof(uint32_t));
    
    uint32_t threads = 256;
    uint32_t blocks = (n_agents + threads - 1) / threads;
    k_count_agents<<<blocks, threads>>>(d_net, n_agents, d_counts);
    cudaMemcpy(h_counts, d_counts, 5 * sizeof(uint32_t), cudaMemcpyDeviceToHost);
    
    *n_reduced = h_counts[0] + h_counts[1] + h_counts[3]; // CON + DUP + SIFT (non-ERA)
    *rounds = round;
    
    // Cleanup
    cudaFree(d_net);
    cudaFree(d_active_pairs);
    cudaFree(d_n_active);
    cudaFree(d_counts);
    
           *n_reduced, h_counts[0], h_counts[1], h_counts[2], h_counts[3], h_counts[4]);
    
    return result_net;
}
