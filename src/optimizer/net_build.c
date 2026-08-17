// bine/net/build.c — Stage 3: Binary → Interaction Net Construction
// Builds CFG, DFG, classifies instructions, wires the net.
// Production-quality implementation: pure input-driven, no external lookups.

#include "bine.h"
#include "icor/bits.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

// ============================================================================
// Data Flow Graph Node
// ============================================================================

typedef struct {
    uint32_t insn_idx;
    uint32_t* consumers;
    uint32_t n_consumers;
    uint32_t* producers;
    uint32_t n_producers;
    uint8_t  def_reg;      // Register defined by this instruction (0xFF = none)
    uint8_t  use_regs[4];  // Registers used by this instruction
    uint8_t  n_use_regs;
} DFGNode;

// ============================================================================
// CFG Construction — Pure Input-Driven
// ============================================================================

// Free all memory associated with a CFG
static void free_cfg(BasicBlock* cfg, uint32_t n_blocks) {
    if (!cfg) return;
    for (uint32_t i = 0; i < n_blocks; i++) {
        free(cfg[i].successors);
        free(cfg[i].predecessors);
    }
    free(cfg);
}

// Add a successor edge from block 'from' to block 'to'
static void add_successor(BasicBlock* blocks, uint32_t from, uint32_t to) {
    // Check for duplicates
    for (uint32_t k = 0; k < blocks[from].n_successors; k++) {
        if (blocks[from].successors[k] == to) return;
    }
    uint32_t* new_succ = (uint32_t*)realloc(blocks[from].successors,
        (blocks[from].n_successors + 1) * sizeof(uint32_t));
    if (!new_succ) return;
    blocks[from].successors = new_succ;
    blocks[from].successors[blocks[from].n_successors++] = to;

    uint32_t* new_pred = (uint32_t*)realloc(blocks[to].predecessors,
        (blocks[to].n_predecessors + 1) * sizeof(uint32_t));
    if (!new_pred) return;
    blocks[to].predecessors = new_pred;
    blocks[to].predecessors[blocks[to].n_predecessors++] = from;
}

// Build control flow graph from decoded instructions — PURE INPUT-DRIVEN
// No external symbol table lookups — only instruction stream analysis.
static BasicBlock* build_cfg_pure(const Instruction* insns, uint32_t n_insns,
                                  uint64_t entry_offset,
                                  uint32_t* n_blocks) {
    if (!insns || n_insns == 0 || !n_blocks) return NULL;

    uint8_t* is_leader = (uint8_t*)calloc(n_insns, sizeof(uint8_t));
    if (!is_leader) return NULL;

    // Mark entry point as leader
    uint32_t entry_insn_idx = n_insns; // sentinel: not found
    for (uint32_t i = 0; i < n_insns; i++) {
        if (insns[i].offset == entry_offset) {
            is_leader[i] = 1;
            entry_insn_idx = i;
            break;
        }
    }
    // If entry point not found, mark first instruction as entry (fallback)
    if (entry_insn_idx >= n_insns) {
        entry_insn_idx = 0;
        is_leader[0] = 1;
    }

    // Mark CALL targets as leaders (internal function calls)
    for (uint32_t i = 0; i < n_insns; i++) {
        if (insns[i].is_call && insns[i].target != 0) {
            for (uint32_t j = 0; j < n_insns; j++) {
                if (insns[j].offset == insns[i].target) {
                    is_leader[j] = 1;
                    break;
                }
            }
        }
    }

    // Mark branch targets as leaders
    for (uint32_t i = 0; i < n_insns; i++) {
        if (insns[i].is_branch && insns[i].target != 0) {
            for (uint32_t j = 0; j < n_insns; j++) {
                if (insns[j].offset == insns[i].target) {
                    is_leader[j] = 1;
                    break;
                }
            }
        }
    }

    // Mark instructions after branches/calls/returns as leaders
    for (uint32_t i = 0; i + 1 < n_insns; i++) {
        if (insns[i].is_branch || insns[i].is_call || insns[i].is_ret) {
            is_leader[i + 1] = 1;
        }
    }

    // Count leaders to determine number of blocks
    *n_blocks = 0;
    for (uint32_t i = 0; i < n_insns; i++) {
        if (is_leader[i]) (*n_blocks)++;
    }
    if (*n_blocks == 0) {
        free(is_leader);
        return NULL;
    }

    BasicBlock* blocks = (BasicBlock*)calloc(*n_blocks, sizeof(BasicBlock));
    if (!blocks) {
        free(is_leader);
        return NULL;
    }

    // Create basic blocks
    uint32_t block_idx = 0;
    uint32_t block_start = 0;
    for (uint32_t i = 0; i < n_insns; i++) {
        if (is_leader[i] && i > block_start) {
            blocks[block_idx].start = insns[block_start].offset;
            blocks[block_idx].end = insns[i - 1].offset + insns[i - 1].length;
            blocks[block_idx].first_insn = block_start;
            blocks[block_idx].last_insn = i - 1;
            block_idx++;
            block_start = i;
        }
    }
    // Last block
    if (block_idx < *n_blocks) {
        blocks[block_idx].start = insns[block_start].offset;
        blocks[block_idx].end = insns[n_insns - 1].offset + insns[n_insns - 1].length;
        blocks[block_idx].first_insn = block_start;
        blocks[block_idx].last_insn = n_insns - 1;
    }

    free(is_leader);

    // Build successor/predecessor relationships
    // Add fall-through edges for blocks that don't end with RET or unconditional JMP
    for (uint32_t i = 0; i + 1 < *n_blocks; i++) {
        uint32_t last = blocks[i].last_insn;
        bool has_fallthrough = true;

        if (insns[last].is_ret) {
            has_fallthrough = false;
        } else if (insns[last].is_branch && !insns[last].is_call) {
            // Unconditional JMP (0xE9 rel32, 0xEB rel8, 0xFF /4, 0x0F 0x8x Jcc is conditional)
            uint8_t op = insns[last].opcode[0];
            if (op == 0xE9 || op == 0xEB) {
                has_fallthrough = false;
            } else if (op == 0xFF) {
                uint8_t reg_field = (insns[last].modrm >> 3) & 7;
                if (reg_field == 4) has_fallthrough = false; // JMP r/m
            }
        }

        if (has_fallthrough) {
            add_successor(blocks, i, i + 1);
        }
    }

    // Add branch targets and CALL targets as successors
    for (uint32_t i = 0; i < *n_blocks; i++) {
        uint32_t last = blocks[i].last_insn;

        if (insns[last].is_branch && insns[last].target != 0) {
            for (uint32_t j = 0; j < *n_blocks; j++) {
                if (insns[last].target >= blocks[j].start &&
                    insns[last].target < blocks[j].end) {
                    add_successor(blocks, i, j);
                    break; // target can only be in one block
                }
            }
        }

        if (insns[last].is_call && insns[last].target != 0) {
            for (uint32_t j = 0; j < *n_blocks; j++) {
                if (insns[last].target >= blocks[j].start &&
                    insns[last].target < blocks[j].end) {
                    add_successor(blocks, i, j);
                    break;
                }
            }
        }
    }

    return blocks;
}

// ============================================================================
// DFG Construction
// ============================================================================

static void free_dfg(DFGNode* dfg, uint32_t n_nodes) {
    if (!dfg) return;
    for (uint32_t i = 0; i < n_nodes; i++) {
        free(dfg[i].consumers);
    }
    free(dfg);
}

// Build data flow graph from decoded instructions.
// Performs conservative local (intra-block) analysis only.
static DFGNode* build_dfg(const Instruction* insns, uint32_t n_insns,
                          const BasicBlock* cfg, uint32_t n_cfg,
                          uint32_t* n_nodes) {
    if (!insns || !cfg || !n_nodes) return NULL;

    *n_nodes = n_insns;
    DFGNode* nodes = (DFGNode*)calloc(*n_nodes, sizeof(DFGNode));
    if (!nodes) return NULL;

    for (uint32_t i = 0; i < n_insns; i++) {
        const Instruction* insn = &insns[i];
        nodes[i].insn_idx = i;
        nodes[i].def_reg = 0xFF;
        nodes[i].n_use_regs = 0;

        switch (insn->opcode[0]) {
            case 0x50: case 0x51: case 0x52: case 0x53:
            case 0x54: case 0x55: case 0x56: case 0x57:
                nodes[i].use_regs[nodes[i].n_use_regs++] = insn->reg;
                break;

            case 0x58: case 0x59: case 0x5A: case 0x5B:
            case 0x5C: case 0x5D: case 0x5E: case 0x5F:
                nodes[i].def_reg = insn->reg;
                break;

            case 0x89:
                nodes[i].use_regs[nodes[i].n_use_regs++] = insn->reg;
                nodes[i].def_reg = insn->rm;
                break;

            case 0x8B:
                nodes[i].def_reg = insn->reg;
                nodes[i].use_regs[nodes[i].n_use_regs++] = insn->rm;
                break;

            case 0xB8: case 0xB9: case 0xBA: case 0xBB:
            case 0xBC: case 0xBD: case 0xBE: case 0xBF:
                nodes[i].def_reg = insn->reg;
                break;

            default:
                break;
        }
    }

    // Build intra-block consumer relationships only
    for (uint32_t b = 0; b < n_cfg; b++) {
        uint32_t first = cfg[b].first_insn;
        uint32_t last  = cfg[b].last_insn;
        if (last >= n_insns) last = n_insns - 1;

        for (uint32_t i = first; i <= last; i++) {
            if (nodes[i].def_reg == 0xFF) continue;

            // Count consumers
            uint32_t count = 0;
            for (uint32_t j = i + 1; j <= last && j < n_insns; j++) {
                bool uses = false;
                for (uint8_t k = 0; k < nodes[j].n_use_regs; k++) {
                    if (nodes[j].use_regs[k] == nodes[i].def_reg) { uses = true; break; }
                }
                if (!uses) continue;

                bool killed = false;
                for (uint32_t k = i + 1; k < j; k++) {
                    if (nodes[k].def_reg == nodes[i].def_reg) { killed = true; break; }
                }
                if (killed) break;
                count++;
            }

            if (count > 0) {
                nodes[i].consumers = (uint32_t*)calloc(count, sizeof(uint32_t));
                if (nodes[i].consumers) {
                    uint32_t filled = 0;
                    for (uint32_t j = i + 1; j <= last && j < n_insns; j++) {
                        bool uses = false;
                        for (uint8_t k = 0; k < nodes[j].n_use_regs; k++) {
                            if (nodes[j].use_regs[k] == nodes[i].def_reg) { uses = true; break; }
                        }
                        if (!uses) continue;

                        bool killed = false;
                        for (uint32_t k = i + 1; k < j; k++) {
                            if (nodes[k].def_reg == nodes[i].def_reg) { killed = true; break; }
                        }
                        if (killed) break;
                        nodes[i].consumers[filled++] = j;
                    }
                    nodes[i].n_consumers = filled;
                }
            }
        }
    }

    return nodes;
}

// ============================================================================
// Instruction Classification
// ============================================================================

// Classify each instruction into an agent type.
// Production policy:
//   - Unreachable code (per the CFG seeded from entry, symbols and intra-.text
//     relocations; see bine_analyze_reachability) is ERA (dead) and eligible
//     for NOP-elimination by the serializer.
//   - All other instructions are preserved conservatively.
static AgentType classify_instruction(const DFGNode* dfg, uint32_t insn_idx,
                                      const Instruction* insn,
                                      const bool* reachable) {
    (void)dfg; // DFG analysis is intra-block only; too unsafe for DCE
    (void)insn_idx;

    // Preserve all control-flow instructions.
    if (insn->is_branch || insn->is_call || insn->is_ret) {
        return AGENT_CON;
    }

    // Constant loads are SIFT agents.
    if ((insn->opcode[0] >= 0xB8 && insn->opcode[0] <= 0xBF)) {
        return AGENT_SIFT;
    }

    // Unreachable code is safe to eliminate: the CFG roots include the entry
    // point, every symbol address, and every intra-.text relocation target,
    // so genuinely live entry points (init arrays, PLT stubs, indirect
    // calls) are never classified as dead.
    if (reachable && !reachable[insn_idx]) {
        return AGENT_ERA;
    }

    // Everything else is conservative.
    return AGENT_CON;
}

// ============================================================================
// Interaction Net Construction
// ============================================================================

uint64_t* bine_build_net(const BinaryIntrinsic* bin, const StrandMasks* masks,
                         uint32_t* n_agents, uint32_t* root_wire) {
    if (!bin || !masks || !n_agents || !root_wire) {
        return NULL;
    }

    uint64_t text_size = bin->text_size;
    if (text_size == 0 || !bin->text_bytes) {
        return NULL;
    }

    // Decode instructions
    uint32_t n_insns = 0;
    Instruction* insns = decode_instructions(bin->text_bytes, text_size, &n_insns);
    if (!insns || n_insns == 0) {
        return NULL;
    }

    // Compute entry point offset within .text section
    uint64_t entry_offset = 0;
    if (bin->text_vaddr != 0) {
        entry_offset = bin->e_entry - bin->text_vaddr;
    } else {
        // Fallback: compute from LOAD segment (legacy, may be wrong if .init/.plt precede .text)
        entry_offset = bin->e_entry - bin->load_vaddr;
    }
    
    // Validate entry offset - if it's outside .text section, start from beginning
    // This handles dynamically linked binaries where main() is called indirectly
    if (entry_offset >= text_size) {
        entry_offset = 0;
    }

    // Build CFG
    uint32_t n_cfg = 0;
    BasicBlock* cfg = build_cfg_pure(insns, n_insns, entry_offset, &n_cfg);
    if (!cfg) {
        free_instructions(insns);
        return NULL;
    }

    // Reachability analysis
    bool* reachable = bine_analyze_reachability(cfg, n_cfg, insns, n_insns, bin);

    
    // Build DFG
    uint32_t n_dfg = 0;
    DFGNode* dfg = build_dfg(insns, n_insns, cfg, n_cfg, &n_dfg);
    if (!dfg) {
        free_cfg(cfg, n_cfg);
        free_instructions(insns);
        free(reachable);
        return NULL;
    }

    // Allocate net
    *n_agents = n_insns;
    uint64_t* net = (uint64_t*)calloc(*n_agents, sizeof(uint64_t));
    if (!net) {
        free_dfg(dfg, n_dfg);
        free_cfg(cfg, n_cfg);
        free_instructions(insns);
        free(reachable);
        return NULL;
    }

    // Classify and pack each instruction
    for (uint32_t i = 0; i < n_insns; i++) {
        AgentType type = classify_instruction(dfg, i, &insns[i], reachable);

        uint64_t word = SET_TYPE(0ULL, (uint64_t)type);

        // Essential agents (CALL/RET/branch) can never be eliminated.
        if (insns[i].is_call || insns[i].is_ret || insns[i].is_branch) {
            word = SET_ESSENTIAL(word);
        }

        if (type == AGENT_SIFT && insns[i].imm != 0) {
            word = SET_PORT(word, 0, (uint32_t)insns[i].imm);
        } else if (dfg[i].def_reg != 0xFF) {
            word = SET_PORT(word, 0, dfg[i].def_reg);
        }

        if (dfg[i].n_use_regs > 0) {
            word = SET_PORT(word, 1, dfg[i].use_regs[0]);
        }

        if (dfg[i].n_consumers > 0 && dfg[i].consumers) {
            word = SET_PORT(word, 2, dfg[i].consumers[0]);
        }

        net[i] = word;
    }

    // Set root wire to entry point instruction
    *root_wire = 0;
    for (uint32_t i = 0; i < n_insns; i++) {
        if (insns[i].offset == entry_offset) {
            *root_wire = i;
            break;
        }
    }

    // Cleanup
    free(reachable);
    free_dfg(dfg, n_dfg);
    free_cfg(cfg, n_cfg);
    free_instructions(insns);

    return net;
}

void bine_free_net(uint64_t* net) {
    free(net);
}
