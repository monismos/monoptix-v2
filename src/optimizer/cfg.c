// bine/net/cfg_analysis.c — Control flow analysis for safe optimization
// Implements reachability analysis to identify truly dead code.

#include "bine.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// Mark all blocks reachable from a given start block using DFS
static void mark_reachable(const BasicBlock* cfg, uint32_t n_cfg,
                           bool* reachable, uint32_t start_block) {
    if (start_block >= n_cfg) return;

    uint32_t* stack = (uint32_t*)malloc(n_cfg * sizeof(uint32_t));
    if (!stack) return;

    uint32_t stack_top = 0;
    stack[stack_top++] = start_block;

    while (stack_top > 0) {
        uint32_t bb_idx = stack[--stack_top];
        if (reachable[bb_idx]) continue;
        reachable[bb_idx] = true;

        const BasicBlock* bb = &cfg[bb_idx];
        for (uint32_t i = 0; i < bb->n_successors; i++) {
            if (bb->successors[i] < n_cfg && !reachable[bb->successors[i]]) {
                stack[stack_top++] = bb->successors[i];
            }
        }
    }

    free(stack);
}

// Mark all instructions that belong to reachable blocks
static void mark_insn_reachable(const BasicBlock* cfg, uint32_t n_cfg,
                                const bool* bb_reachable,
                                bool* insn_reachable,
                                const Instruction* insns,
                                uint32_t n_insns) {
    (void)insns;
    memset(insn_reachable, 0, n_insns * sizeof(bool));

    for (uint32_t i = 0; i < n_cfg; i++) {
        if (!bb_reachable[i]) continue;
        const BasicBlock* bb = &cfg[i];
        for (uint32_t j = bb->first_insn; j <= bb->last_insn && j < n_insns; j++) {
            insn_reachable[j] = true;
        }
    }
}

// Find the basic block containing the entry point offset
static uint32_t find_entry_block(const BasicBlock* cfg, uint32_t n_cfg,
                                 uint64_t entry_offset) {
    for (uint32_t i = 0; i < n_cfg; i++) {
        if (cfg[i].start <= entry_offset && entry_offset < cfg[i].end) {
            return i;
        }
    }
    // Fallback: return block closest to entry offset
    uint32_t closest = 0;
    uint64_t min_dist = UINT64_MAX;
    for (uint32_t i = 0; i < n_cfg; i++) {
        uint64_t dist = (entry_offset > cfg[i].start)
                        ? (entry_offset - cfg[i].start)
                        : (cfg[i].start - entry_offset);
        if (dist < min_dist) {
            min_dist = dist;
            closest = i;
        }
    }
    return closest;
}

// Main reachability analysis function
bool* bine_analyze_reachability(const BasicBlock* cfg, uint32_t n_cfg,
                                const Instruction* insns, uint32_t n_insns,
                                const BinaryIntrinsic* bin) {
    if (!cfg || n_cfg == 0 || !insns || n_insns == 0 || !bin) {
        return NULL;
    }

    bool* bb_reachable = (bool*)calloc(n_cfg, sizeof(bool));
    bool* insn_reachable = (bool*)calloc(n_insns, sizeof(bool));

    if (!bb_reachable || !insn_reachable) {
        free(bb_reachable);
        free(insn_reachable);
        return NULL;
    }

    // Reachability is seeded from the ELF entry point and from function
    // symbols inside .text.  A valid entry is always required (falling back
    // to the first instruction); we never mark everything reachable, or
    // dead-code elimination could never fire on stripped binaries.
    uint64_t entry_offset = 0;
    if (bin->e_entry != 0) {
        uint64_t base = (bin->text_vaddr != 0) ? bin->text_vaddr : bin->load_vaddr;
        if (base != 0 && bin->e_entry >= base &&
            bin->e_entry < base + bin->text_size) {
            entry_offset = bin->e_entry - base;
        }
    }

    uint32_t entry_block = find_entry_block(cfg, n_cfg, entry_offset);
    mark_reachable(cfg, n_cfg, bb_reachable, entry_block);

    // Also mark all function/notype symbols within .text as reachable roots
    for (uint32_t i = 0; i < bin->n_symbols; i++) {
        const Symbol* sym = &bin->symbols[i];
        // type 2 is STT_FUNC, type 0 is STT_NOTYPE
        if (sym->type == 2 || sym->type == 0) {
            uint64_t sym_offset = 0;
            bool in_range = false;
            if (bin->text_vaddr != 0) {
                if (sym->address >= bin->text_vaddr && sym->address < bin->text_vaddr + bin->text_size) {
                    sym_offset = sym->address - bin->text_vaddr;
                    in_range = true;
                }
            } else {
                if (sym->address >= bin->load_vaddr && sym->address < bin->load_vaddr + bin->text_size) {
                    sym_offset = sym->address - bin->load_vaddr;
                    in_range = true;
                }
            }

            if (in_range) {
                uint32_t sym_block = find_entry_block(cfg, n_cfg, sym_offset);
                mark_reachable(cfg, n_cfg, bb_reachable, sym_block);
            }
        }
    }

    mark_insn_reachable(cfg, n_cfg, bb_reachable, insn_reachable, insns, n_insns);

    free(bb_reachable);
    return insn_reachable;
}
