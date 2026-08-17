// bine/gpu/register_assign.cu — GPU Tree-Coloring Register Assignment
// Assigns physical registers to wires in the ICOR normal form net.

#include "bine.h"
#include <cuda_runtime.h>
#include <device_launch_parameters.h>
#include <stdio.h>

// Wire interference graph node
struct WireNode {
    uint32_t wire_id;
    uint32_t* interfering_wires;
    uint32_t n_interfering;
    uint32_t assigned_reg;
};

// GPU kernel: build wire interference graph
__global__ void k_build_interference_graph(uint64_t* net, uint32_t n_agents,
                                             uint32_t* interference,
                                             const StrandMasks* masks) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= n_agents) return;
    
    AgentPacked ap = *(AgentPacked*)&net[tid];
    
    // Check interference with all other wires
    for (uint32_t j = 0; j < n_agents; j++) {
        if (tid != j) {
            AgentPacked bp = *(AgentPacked*)&net[j];
            
            // Wires interfere if they are live simultaneously
            // Simplified: check if they share ports
            bool interferes = (ap.port_0 == bp.port_0) ||
                            (ap.port_1 == bp.port_1) ||
                            (ap.port_2 == bp.port_2);
            
            if (interferes) {
                interference[tid * n_agents + j] = 1;
            } else {
                interference[tid * n_agents + j] = 0;
            }
        }
    }
}

// GPU kernel: greedy tree coloring
__global__ void k_tree_coloring(uint32_t* interference, uint32_t n_agents,
                                 uint32_t* wire_to_reg, uint32_t n_phys_regs) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= n_agents) return;
    
    // Try each color (register) in order
    for (uint32_t reg = 0; reg < n_phys_regs; reg++) {
        bool can_use = true;
        
        // Check if any interfering wire uses this register
        for (uint32_t j = 0; j < n_agents; j++) {
            if (tid != j && interference[tid * n_agents + j]) {
                if (wire_to_reg[j] == reg) {
                    can_use = false;
                    break;
                }
            }
        }
        
        if (can_use) {
            wire_to_reg[tid] = reg;
            return;
        }
    }
    
    // No register available → spill (use register 0 as fallback)
    wire_to_reg[tid] = 0;
}

// Main register assignment function
uint32_t* bine_assign_registers(const uint64_t* net, uint32_t n_agents,
                                const BinaryIntrinsic* bin,
                                uint32_t* n_assigned) {
    if (!net || n_agents == 0 || !bin) {
        *n_assigned = 0;
        return NULL;
    }
    
    // Determine number of physical registers based on architecture
    uint32_t n_phys_regs;
    switch (bin->e_machine) {
        case EM_X86_64:
            n_phys_regs = 16; // General-purpose registers
            break;
        case EM_AARCH64:
            n_phys_regs = 31; // X0-X30
            break;
        case EM_RISCV:
            n_phys_regs = 32; // X0-X31
            break;
        default:
            n_phys_regs = 16;
            break;
    }
    
    // Allocate device memory
    uint64_t* d_net;
    uint32_t* d_interference;
    uint32_t* d_wire_to_reg;
    
    cudaMalloc(&d_net, n_agents * sizeof(uint64_t));
    cudaMalloc(&d_interference, n_agents * n_agents * sizeof(uint32_t));
    cudaMalloc(&d_wire_to_reg, n_agents * sizeof(uint32_t));
    
    cudaMemcpy(d_net, net, n_agents * sizeof(uint64_t), cudaMemcpyHostToDevice);
    cudaMemset(d_interference, 0, n_agents * n_agents * sizeof(uint32_t));
    cudaMemset(d_wire_to_reg, 0xFFFFFFFF, n_agents * sizeof(uint32_t));
    
    uint32_t threads = 256;
    uint32_t blocks = (n_agents + threads - 1) / threads;
    
    // Build interference graph
    k_build_interference_graph<<<blocks, threads>>>(d_net, n_agents, d_interference, NULL);
    cudaDeviceSynchronize();
    
    // Tree coloring
    k_tree_coloring<<<blocks, threads>>>(d_interference, n_agents, d_wire_to_reg, n_phys_regs);
    cudaDeviceSynchronize();
    
    // Copy result back
    uint32_t* wire_to_reg = (uint32_t*)malloc(n_agents * sizeof(uint32_t));
    cudaMemcpy(wire_to_reg, d_wire_to_reg, n_agents * sizeof(uint32_t), cudaMemcpyDeviceToHost);
    
    *n_assigned = n_agents;
    
    // Cleanup
    cudaFree(d_net);
    cudaFree(d_interference);
    cudaFree(d_wire_to_reg);
    
    
    return wire_to_reg;
}
