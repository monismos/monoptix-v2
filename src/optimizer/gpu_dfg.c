// bine/net/gpu_dfg.c ??? GPU Data Flow Graph Construction
// Builds DFG for GPU kernels considering SIMT execution model

#include "bine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// GPU-specific DFG node
typedef struct {
    uint64_t insn_idx;
    uint32_t kernel_id;
    uint32_t* use_regs;
    uint32_t* def_regs;
    uint32_t n_use;
    uint32_t n_def;
    uint32_t* consumers;
    uint32_t n_consumers;
    uint32_t* producers;
    uint32_t n_producers;
} GPUDataFlowNode;


// Build GPU DFG from instructions
GPUDFG* bine_build_gpu_dfg(const GPUIntrinsic* gpu,
                          const Instruction* insns, uint32_t n_insns) {
    GPUDFG* dfg = (GPUDFG*)calloc(1, sizeof(GPUDFG));
    if (!dfg) return NULL;
    
    dfg->n_nodes = n_insns;
    dfg->n_kernels = gpu ? gpu->n_kernels : 0;
    dfg->nodes = calloc(n_insns, sizeof(*dfg->nodes));
    if (!dfg->nodes) {
        free(dfg);
        return NULL;
    }

    if (dfg->n_kernels > 0) {
        dfg->kernel_offsets = (uint32_t*)calloc(dfg->n_kernels, sizeof(uint32_t));
        if (!dfg->kernel_offsets) {
            bine_free_gpu_dfg(dfg);
            return NULL;
        }

        uint32_t offset = 0;
        for (uint32_t k = 0; k < dfg->n_kernels; k++) {
            dfg->kernel_offsets[k] = offset;
            offset += gpu->kernels[k].insn_count;
        }
    }
    
    // Initialize nodes
    for (uint32_t i = 0; i < n_insns; i++) {
        dfg->nodes[i].insn_idx = i;
        dfg->nodes[i].use_regs = (uint32_t*)calloc(16, sizeof(uint32_t));
        dfg->nodes[i].def_regs = (uint32_t*)calloc(1, sizeof(uint32_t));
        if (!dfg->nodes[i].use_regs || !dfg->nodes[i].def_regs) {
            bine_free_gpu_dfg(dfg);
            return NULL;
        }

        dfg->nodes[i].n_use = insns[i].n_use_regs > 16 ? 16 : insns[i].n_use_regs;
        dfg->nodes[i].n_def = (insns[i].def_reg != UINT32_MAX) ? 1 : 0;

        if (dfg->n_kernels > 0) {
            uint32_t kid = dfg->n_kernels - 1;
            for (uint32_t k = 0; k < dfg->n_kernels; k++) {
                uint32_t start = dfg->kernel_offsets[k];
                uint32_t end = start + gpu->kernels[k].insn_count;
                if (i >= start && i < end) {
                    kid = k;
                    break;
                }
            }
            dfg->nodes[i].kernel_id = kid;
        }
        
        // Copy register usage
        for (uint32_t j = 0; j < dfg->nodes[i].n_use; j++) {
            dfg->nodes[i].use_regs[j] = insns[i].use_regs[j];
        }
        
        if (dfg->nodes[i].n_def > 0) {
            dfg->nodes[i].def_regs[0] = insns[i].def_reg;
        }
    }
    
    // Build producer-consumer relationships
    for (uint32_t i = 0; i < n_insns; i++) {
        GPUDFGNode* node = &dfg->nodes[i];
        
        // For each used register, find producer
        for (uint32_t j = 0; j < node->n_use; j++) {
            uint32_t used_reg = node->use_regs[j];
            
            // Search backwards for producer
            for (uint32_t k = i; k-- > 0;) {
                if (dfg->nodes[k].n_def > 0 && dfg->nodes[k].def_regs[0] == used_reg) {
                    // Found producer
                    uint32_t next_consumers = dfg->nodes[k].n_consumers + 1;
                    uint32_t* consumers = (uint32_t*)realloc(
                        dfg->nodes[k].consumers,
                        next_consumers * sizeof(uint32_t));
                    if (!consumers) {
                        bine_free_gpu_dfg(dfg);
                        return NULL;
                    }
                    dfg->nodes[k].consumers = consumers;
                    dfg->nodes[k].consumers[next_consumers - 1] = i;
                    dfg->nodes[k].n_consumers = next_consumers;
                    
                    uint32_t next_producers = dfg->nodes[i].n_producers + 1;
                    uint32_t* producers = (uint32_t*)realloc(
                        dfg->nodes[i].producers,
                        next_producers * sizeof(uint32_t));
                    if (!producers) {
                        bine_free_gpu_dfg(dfg);
                        return NULL;
                    }
                    dfg->nodes[i].producers = producers;
                    dfg->nodes[i].producers[next_producers - 1] = k;
                    dfg->nodes[i].n_producers = next_producers;
                    break;
                }
            }
        }
    }
    
    printf("[GPU-DFG] Built DFG with %u nodes\n", n_insns);
    return dfg;
}

// Free GPU DFG
void bine_free_gpu_dfg(GPUDFG* dfg) {
    if (!dfg) return;
    
    if (dfg->nodes) {
        for (uint32_t i = 0; i < dfg->n_nodes; i++) {
            free(dfg->nodes[i].use_regs);
            free(dfg->nodes[i].def_regs);
            free(dfg->nodes[i].consumers);
            free(dfg->nodes[i].producers);
        }
        free(dfg->nodes);
    }
    
    free(dfg->kernel_offsets);
    free(dfg);
}

