// bine/net/gpu_net.c — GPU Interaction Net Construction
// Builds interaction nets from GPU instruction data for GPU-ICOR reduction

#include "bine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// GPU-specific agent classification
typedef enum {
    GPU_AGENT_CON,     // Computation (arithmetic, logic)
    GPU_AGENT_LD,       // Load from memory
    GPU_AGENT_ST,       // Store to memory
    GPU_AGENT_BRA,      // Branch control flow
    GPU_AGENT_CALL,     // Function call
    GPU_AGENT_RET,      // Return
    GPU_AGENT_BAR,      // Barrier (synchronization)
    GPU_AGENT_MOV,      // Register move
    GPU_AGENT_ERA       // Dead code
} GPUAgentType;

// GPU interaction net agent
typedef struct {
    uint64_t id;
    GPUAgentType type;
    uint32_t kernel_id;
    uint64_t insn_idx;
    uint8_t essential;    // Cannot be eliminated
    uint8_t simt_aware;   // Affects all threads
    uint32_t wire_0;      // Primary input/output wire
    uint32_t wire_1;      // Secondary input/output wire
    uint32_t wire_2;      // Tertiary input/output wire
    uint32_t consumers[8]; // Consumer agents
    uint32_t n_consumers;
} GPUAgent;


// Classify GPU instruction to agent type
static GPUAgentType classify_gpu_insn(const Instruction* insn, 
                                   const typeof(*((GPUDFG*)0)->nodes)* dfg_node,
                                   bool reachable) {
    // Unreachable instructions become ERA
    if (!reachable) return GPU_AGENT_ERA;
    
    // Control flow instructions
    if (insn->is_branch) return GPU_AGENT_BRA;
    if (insn->is_call) return GPU_AGENT_CALL;
    if (insn->is_ret) return GPU_AGENT_RET;
    
    // Memory operations (check opcode)
    if (strncmp(insn->opcode, "ld", 2) == 0) return GPU_AGENT_LD;
    if (strncmp(insn->opcode, "st", 2) == 0) return GPU_AGENT_ST;
    if (strncmp(insn->opcode, "bar", 3) == 0) return GPU_AGENT_BAR;
    if (strncmp(insn->opcode, "mov", 3) == 0) return GPU_AGENT_MOV;
    
    // Arithmetic/logic operations
    if (strncmp(insn->opcode, "add", 3) == 0 ||
        strncmp(insn->opcode, "sub", 3) == 0 ||
        strncmp(insn->opcode, "mul", 3) == 0 ||
        strncmp(insn->opcode, "mad", 3) == 0 ||
        strncmp(insn->opcode, "div", 3) == 0 ||
        strncmp(insn->opcode, "and", 3) == 0 ||
        strncmp(insn->opcode, "or", 2) == 0 ||
        strncmp(insn->opcode, "xor", 3) == 0) {
        return GPU_AGENT_CON;
    }
    
    // Default to computation
    return GPU_AGENT_CON;
}

// Determine if instruction is essential (cannot be eliminated)
static uint8_t is_essential_gpu_insn(const Instruction* insn, GPUAgentType type) {
    // Control flow is always essential
    if (type == GPU_AGENT_BRA || type == GPU_AGENT_CALL || type == GPU_AGENT_RET) {
        return 1;
    }
    
    // Barriers are essential for correctness
    if (type == GPU_AGENT_BAR) {
        return 1;
    }
    
    // Memory operations with side effects are essential
    if (type == GPU_AGENT_ST) {
        return 1;
    }
    
    // Loads from global memory might be essential
    if (type == GPU_AGENT_LD && strstr(insn->opcode, "global")) {
        return 1;
    }
    
    return 0;
}

static uint8_t can_erase_unused_gpu_insn(GPUAgentType type) {
    return type == GPU_AGENT_CON ||
           type == GPU_AGENT_LD ||
           type == GPU_AGENT_MOV;
}

static uint32_t gpu_kernel_for_insn(const GPUIntrinsic* gpu, uint32_t insn_idx) {
    if (!gpu || !gpu->kernels || gpu->n_kernels == 0) {
        return 0;
    }

    uint64_t base = 0;
    for (uint32_t k = 0; k < gpu->n_kernels; k++) {
        uint64_t lim = base + (uint64_t)gpu->kernels[k].insn_count;
        if ((uint64_t)insn_idx < lim) {
            return k;
        }
        base = lim;
    }

    return gpu->n_kernels - 1;
}

// Build GPU interaction net from instructions
GPUNet* bine_build_gpu_net(const GPUIntrinsic* gpu,
                          const Instruction* insns, uint32_t n_insns,
                          const GPUDFG* dfg,
                          const bool* reachable_insns) {
    GPUNet* net = (GPUNet*)calloc(1, sizeof(GPUNet));
    if (!net) return NULL;
    
    net->n_agents = n_insns;
    net->agents = calloc(n_insns, sizeof(*net->agents));
    if (!net->agents) {
        free(net);
        return NULL;
    }
    
    // Build agents from instructions
    for (uint32_t i = 0; i < n_insns; i++) {
        typeof(*net->agents) *agent = &net->agents[i];
        const Instruction* insn = &insns[i];
        
        agent->id = i;
        agent->insn_idx = i;
        agent->type = classify_gpu_insn(insn, 
                                      dfg ? &dfg->nodes[i] : NULL,
                                      reachable_insns ? reachable_insns[i] : true);
        agent->essential = is_essential_gpu_insn(insn, agent->type);
        if (!agent->essential &&
            dfg &&
            can_erase_unused_gpu_insn(agent->type) &&
            insn->def_reg != UINT32_MAX &&
            dfg->nodes[i].n_consumers == 0) {
            agent->type = GPU_AGENT_ERA;
        }
        agent->simt_aware = 1; // All GPU instructions affect all threads in warp
        
        // Assign wires based on register dependencies
        if (insn->def_reg != UINT32_MAX) {
            agent->wire_0 = insn->def_reg; // Primary output
        }
        
        if (insn->n_use_regs > 0) {
            agent->wire_1 = insn->use_regs[0]; // Primary input
        }
        
        if (insn->n_use_regs > 1) {
            agent->wire_2 = insn->use_regs[1]; // Secondary input
        }
        
        agent->kernel_id = gpu_kernel_for_insn(gpu, i);
    }
    
    // Build consumer relationships from DFG
    if (dfg) {
        for (uint32_t i = 0; i < n_insns; i++) {
            const typeof(*((GPUDFG*)0)->nodes)* dfg_node = &dfg->nodes[i];
            typeof(*net->agents) *agent = &net->agents[i];
            
            agent->n_consumers = dfg_node->n_consumers;
            for (uint32_t j = 0; j < dfg_node->n_consumers && j < 8; j++) {
                agent->consumers[j] = dfg_node->consumers[j];
            }
        }
    }
    
    // Create wire-to-agent mapping for reduction
    net->wire_to_agent = (uint64_t*)malloc(1024 * sizeof(uint64_t)); // Support up to 1024 registers
    if (!net->wire_to_agent) {
        free(net->agents);
        free(net);
        return NULL;
    }
    for (uint32_t i = 0; i < 1024; i++) {
        net->wire_to_agent[i] = UINT64_MAX;
    }
    
    for (uint32_t i = 0; i < n_insns; i++) {
        const typeof(*net->agents) *agent = &net->agents[i];
        
        if (agent->wire_0 < 1024) {
            net->wire_to_agent[agent->wire_0] = i;
        }
        if (agent->wire_1 < 1024) {
            net->wire_to_agent[agent->wire_1] = i;
        }
        if (agent->wire_2 < 1024) {
            net->wire_to_agent[agent->wire_2] = i;
        }
    }
    
    // Set root wire to first non-ERA agent
    for (uint32_t i = 0; i < n_insns; i++) {
        if (net->agents[i].type != GPU_AGENT_ERA) {
            net->root_wire = net->agents[i].wire_0;
            break;
        }
    }
    
    printf("[GPU-NET] Built net with %u agents\n", n_insns);
    
    // Count agent types
    uint32_t type_counts[10] = {0};
    for (uint32_t i = 0; i < n_insns; i++) {
        type_counts[net->agents[i].type]++;
    }
    
    printf("[GPU-NET] Agent distribution:\n");
    printf("  CON: %u\n", type_counts[GPU_AGENT_CON]);
    printf("  LD:  %u\n", type_counts[GPU_AGENT_LD]);
    printf("  ST:  %u\n", type_counts[GPU_AGENT_ST]);
    printf("  BRA: %u\n", type_counts[GPU_AGENT_BRA]);
    printf("  CALL: %u\n", type_counts[GPU_AGENT_CALL]);
    printf("  RET: %u\n", type_counts[GPU_AGENT_RET]);
    printf("  BAR: %u\n", type_counts[GPU_AGENT_BAR]);
    printf("  MOV: %u\n", type_counts[GPU_AGENT_MOV]);
    printf("  ERA: %u\n", type_counts[GPU_AGENT_ERA]);
    
    return net;
}

// Convert GPU net to packed format for GPU-ICOR
uint64_t* bine_pack_gpu_net(const GPUNet* net, uint32_t* n_packed) {
    if (!net || !n_packed) return NULL;
    
    *n_packed = net->n_agents;
    uint64_t* packed = (uint64_t*)calloc(net->n_agents, sizeof(uint64_t));
    if (!packed) return NULL;
    
    for (uint32_t i = 0; i < net->n_agents; i++) {
        const typeof(*net->agents) *agent = &net->agents[i];
        
        // Pack agent into 64 bits
        packed[i] = 0;
        packed[i] |= ((uint64_t)agent->type & 0xF) << 0;      // Bits 0-3: type
        packed[i] |= ((uint64_t)agent->essential & 0x1) << 4;  // Bit 4: essential
        packed[i] |= ((uint64_t)agent->simt_aware & 0x1) << 5; // Bit 5: SIMT-aware
        packed[i] |= ((uint64_t)agent->kernel_id & 0xFF) << 6; // Bits 6-13: kernel ID
        packed[i] |= ((uint64_t)agent->wire_0 & 0x3FF) << 14; // Bits 14-23: wire 0
        packed[i] |= ((uint64_t)agent->wire_1 & 0x3FF) << 24; // Bits 24-33: wire 1
        packed[i] |= ((uint64_t)agent->wire_2 & 0x3FF) << 34; // Bits 34-43: wire 2
        packed[i] |= ((uint64_t)agent->n_consumers & 0xF) << 44; // Bits 44-47: consumer count
        packed[i] |= ((uint64_t)agent->insn_idx & 0xFFFF) << 48; // Bits 48-63: instruction index
    }
    
    return packed;
}

// Free GPU net
void bine_free_gpu_net(GPUNet* net) {
    if (!net) return;
    
    free(net->agents);
    free(net->wire_to_agent);
    free(net);
}
