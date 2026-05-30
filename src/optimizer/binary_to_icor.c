// bine/net/binary_to_icor.c — Complete Binary to ICOR Conversion
// Converts arbitrary binary data to interaction net representation

#include "bine.h"
#include "apex.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Convert a single instruction to ICOR agents
// Maps instruction semantics to CON/DUP/ERA agents
static uint32_t instruction_to_agents(const uint64_t instruction, uint64_t* agents,
                                      uint32_t max_agents, uint32_t* agent_offset) {
    uint32_t offset = *agent_offset;
    uint32_t agents_created = 0;
    
    // Extract instruction fields (simplified for x86-64)
    uint8_t opcode = (instruction >> 0) & 0xFF;
    uint8_t modrm = (instruction >> 8) & 0xFF;
    uint8_t reg = (modrm >> 3) & 0x07;
    uint8_t rm = modrm & 0x07;
    
    // Create CON agent for the instruction itself
    // CON represents computation that produces output from inputs
    if (offset + 1 < max_agents) {
        agents[offset] = 0x0001000000000000ULL | // CON type
                        ((uint64_t)opcode << 48) | // opcode in data field
                        ((uint64_t)reg << 40) |     // register
                        ((uint64_t)rm << 32);       // addressing mode
        offset++;
        agents_created++;
    }
    
    // Create DUP agents for register dependencies
    // DUP represents sharing - if a register is read, create DUP
    if (offset + 1 < max_agents) {
        agents[offset] = 0x0002000000000000ULL | // DUP type
                        ((uint64_t)reg << 48);     // register being shared
        offset++;
        agents_created++;
    }
    
    // Create ERA agents for dead code detection
    // ERA represents discarded computation
    if (offset + 1 < max_agents) {
        agents[offset] = 0x0003000000000000ULL; // ERA type
        offset++;
        agents_created++;
    }
    
    *agent_offset = offset;
    return agents_created;
}

// Build control flow graph from binary
static void build_cfg_from_binary(const uint8_t* binary, size_t size,
                                  uint32_t* cfg_edges, uint32_t n_edges) {
    // Simplified CFG construction
    // Real implementation would decode all branches and build graph
    for (size_t i = 0; i < size && i < n_edges; i++) {
        cfg_edges[i] = i + 1;  // Linear flow for now
    }
}

// Convert binary data stream to ICOR net
// This is the core "data as computation" conversion
uint64_t* binary_to_icor_net(const uint8_t* binary, size_t size, uint32_t* n_agents_out) {
    if (!binary || size == 0 || !n_agents_out) {
        return NULL;
    }
    if (size > UINT32_MAX) {
        return NULL;
    }
    
    printf("[BINARY->ICOR] Converting %zu bytes to interaction net\n", size);
    
    // Lossless baseline: each byte is a SIFT-style leaf agent. Higher-level
    // semantic lowering can replace this later, but this path must preserve
    // arbitrary binary data exactly today.
    uint32_t total_agents = (uint32_t)size;
    uint64_t* agents = (uint64_t*)calloc(total_agents, sizeof(uint64_t));
    if (!agents) {
        return NULL;
    }
    
    for (uint32_t i = 0; i < total_agents; i++) {
        agents[i] = 0x0004000000000000ULL | (uint64_t)binary[i];
    }
    
    printf("[BINARY->ICOR] Created %u agents from %zu bytes\n", total_agents, size);
    
    *n_agents_out = total_agents;
    return agents;
}

// Convert ICOR net back to binary (inverse operation)
uint8_t* icor_net_to_binary(const uint64_t* net, uint32_t n_agents, size_t* size_out) {
    if (!net || n_agents == 0 || !size_out) {
        return NULL;
    }
    
    printf("[ICOR->BINARY] Converting %u agents to binary\n", n_agents);
    
    uint8_t* binary = (uint8_t*)calloc(n_agents, sizeof(uint8_t));
    if (!binary) {
        return NULL;
    }
    
    size_t binary_offset = 0;
    
    // Reconstruct bytes from SIFT agents. Keep compatibility with the older
    // CON-agent prototype so old test fixtures can still be inspected.
    for (uint32_t i = 0; i < n_agents; i++) {
        uint64_t agent = net[i];
        uint32_t type = (agent >> 48) & 0xFFFF;
        
        if (type == 4) {
            binary[binary_offset++] = (uint8_t)(agent & 0xFF);
        } else if (type == 1) {  // CON agent
            // Extract instruction fields
            uint8_t opcode = (agent >> 0) & 0xFF;
            uint8_t reg = (agent >> 40) & 0xFF;
            uint8_t rm = (agent >> 32) & 0xFF;
            
            // Reconstruct instruction
            uint64_t instruction = opcode | ((uint64_t)reg << 8) | ((uint64_t)rm << 16);
            
            if (binary_offset + 8 <= n_agents) {
                memcpy(binary + binary_offset, &instruction, 8);
                binary_offset += 8;
            }
        }
    }
    
    *size_out = binary_offset;
    printf("[ICOR->BINARY] Reconstructed %zu bytes from %u agents\n", binary_offset, n_agents);
    
    return binary;
}

// Apply GPU-ICOR reduction to binary
// This is the main entry point for the full pipeline
uint8_t* bine_optimize_binary(const uint8_t* input, size_t input_size,
                               size_t* output_size, float* reduction_ratio) {
    if (!input || input_size == 0 || !output_size || !reduction_ratio) {
        return NULL;
    }
    
    
    // Step 1: Convert binary to ICOR net
    uint32_t n_agents = 0;
    uint64_t* net = binary_to_icor_net(input, input_size, &n_agents);
    if (!net || n_agents == 0) {
        return NULL;
    }
    
    // Step 2: Apply APEX compression to the net for side-band size analysis.
    // The framed APEX output is not an agent array, so keep reducing the net.
    APEXResult compressed = apex_compress((const uint8_t*)net, n_agents * sizeof(uint64_t));
    apex_result_free(&compressed);
    
    // Step 3: Apply GPU-ICOR reduction (would use CUDA kernels)
    // For now, apply simple dead code elimination
    uint32_t n_era = 0;
    for (uint32_t i = 0; i < n_agents; i++) {
        if ((net[i] & 0xFFFF000000000000ULL) == 0x0003000000000000ULL) {
            n_era++;
        }
    }
    
    
    // Remove ERA agents
    uint64_t* reduced_net = (uint64_t*)malloc((n_agents - n_era) * sizeof(uint64_t));
    uint32_t reduced_idx = 0;
    for (uint32_t i = 0; i < n_agents; i++) {
        if ((net[i] & 0xFFFF000000000000ULL) != 0x0003000000000000ULL) {
            reduced_net[reduced_idx++] = net[i];
        }
    }
    
    free(net);
    net = reduced_net;
    n_agents = reduced_idx;

    // Step 4: Convert back to binary
    uint8_t* output = icor_net_to_binary(net, n_agents, output_size);
    free(net);
    
    if (!output) {
        return NULL;
    }
    
    *reduction_ratio = (float)*output_size / input_size;

    return output;
}
