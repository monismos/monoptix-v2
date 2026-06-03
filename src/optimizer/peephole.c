// bine/serialize/peephole.c — Peephole Optimization Passes for x86-64
// In-place instruction stream optimizations: NOP consolidation,
// redundant MOV/PUSH-POP elimination, strength reduction.
// All transformations are size-preserving or size-reducing (NOP fill).

#include "bine.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

// Multi-byte NOP encodings (same table as net_to_elf.c)
static const uint8_t nop_enc[15][15] = {
    {0x90},
    {0x66, 0x90},
    {0x0F, 0x1F, 0x00},
    {0x0F, 0x1F, 0x40, 0x00},
    {0x0F, 0x1F, 0x44, 0x00, 0x00},
    {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},
    {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},
    {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x66, 0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
};

// Emit optimal multi-byte NOP of exactly 'len' bytes
static void peep_emit_nop(uint8_t* dst, uint32_t len) {
    while (len > 0) {
        uint32_t n = (len > 15) ? 15 : len;
        memcpy(dst, nop_enc[n - 1], n);
        dst += n;
        len -= n;
    }
}

// Check if byte at offset is a NOP (1-byte 0x90 or multi-byte 0F 1F xx)
static bool is_nop_byte_start(const uint8_t* text, uint64_t offset, uint64_t size,
                               uint32_t* nop_len_out) {
    if (offset >= size) return false;

    // 0x90 = 1-byte NOP
    if (text[offset] == 0x90) {
        *nop_len_out = 1;
        return true;
    }
    // 0x66 prefix chain followed by 0x90 or 0x0F 0x1F
    if (text[offset] == 0x66) {
        // Count 0x66 prefixes
        uint32_t n66 = 0;
        while (offset + n66 < size && text[offset + n66] == 0x66) n66++;
        if (offset + n66 < size && text[offset + n66] == 0x90) {
            *nop_len_out = n66 + 1;
            return true;
        }
        if (offset + n66 + 1 < size &&
            text[offset + n66] == 0x0F && text[offset + n66 + 1] == 0x1F) {
            // Multi-byte NOP: 0x66* 0x0F 0x1F [modrm] [sib] [disp]
            uint32_t base = n66 + 2; // past 0x0F 0x1F
            if (offset + base < size) {
                uint8_t modrm = text[offset + base];
                uint8_t mod = (modrm >> 6) & 3;
                uint8_t rm = modrm & 7;
                uint32_t total = base + 1; // modrm
                if (rm == 4 && mod != 3) total++; // SIB
                if (mod == 1) total += 1;
                else if (mod == 2) total += 4;
                else if (mod == 0 && rm == 5) total += 4;
                *nop_len_out = total;
                return true;
            }
        }
    }
    // 0x0F 0x1F = multi-byte NOP (no 66 prefix)
    if (offset + 1 < size && text[offset] == 0x0F && text[offset + 1] == 0x1F) {
        uint32_t base = 2;
        if (offset + base < size) {
            uint8_t modrm = text[offset + base];
            uint8_t mod = (modrm >> 6) & 3;
            uint8_t rm = modrm & 7;
            uint32_t total = base + 1;
            if (rm == 4 && mod != 3) total++;
            if (mod == 1) total += 1;
            else if (mod == 2) total += 4;
            else if (mod == 0 && rm == 5) total += 4;
            *nop_len_out = total;
            return true;
        }
    }
    return false;
}

// Build a set of branch target offsets from decoded instructions
static bool* build_branch_target_set(const Instruction* insns, uint32_t n_insns,
                                      uint64_t text_size) {
    bool* targets = (bool*)calloc(text_size, sizeof(bool));
    if (!targets) return NULL;

    for (uint32_t i = 0; i < n_insns; i++) {
        if ((insns[i].is_branch || insns[i].is_call) && insns[i].target < text_size) {
            targets[insns[i].target] = true;
        }
    }
    return targets;
}

// ============================================================================
// Pass 1: Redundant MOV elimination (MOV reg, reg where src == dst)
// ============================================================================
static uint32_t pass_redundant_mov(uint8_t* text, uint64_t text_size,
                                    const Instruction* insns, uint32_t n_insns,
                                    const bool* branch_targets) {
    uint32_t eliminated = 0;
    for (uint32_t i = 0; i < n_insns; i++) {
        const Instruction* insn = &insns[i];
        // Skip if this is a branch target (someone jumps here)
        if (branch_targets && insn->offset < text_size && branch_targets[insn->offset])
            continue;

        uint8_t op = insn->opcode[0];
        if ((op == 0x89 || op == 0x8B) && insn->has_modrm) {
            uint8_t modrm = insn->modrm;
            uint8_t mod = (modrm >> 6) & 3;
            uint8_t reg = (modrm >> 3) & 7;
            uint8_t rm = modrm & 7;

            // Apply REX.R and REX.B extensions
            if (insn->rex) {
                if (insn->rex & 0x04) reg |= 8; // REX.R
                if (insn->rex & 0x01) rm |= 8;  // REX.B
            }

            // mod=3 means register-to-register; reg==rm means identity move
            if (mod == 3 && reg == rm) {
                uint64_t off = insn->offset;
                uint32_t len = insn->length;
                if (off + len <= text_size) {
                    peep_emit_nop(text + off, len);
                    eliminated++;
                }
            }
        }
    }
    return eliminated;
}

// ============================================================================
// Pass 2: Redundant PUSH/POP pair elimination
// ============================================================================
static uint32_t pass_push_pop_pairs(uint8_t* text, uint64_t text_size,
                                     const Instruction* insns, uint32_t n_insns,
                                     const bool* branch_targets) {
    uint32_t eliminated = 0;
    for (uint32_t i = 0; i + 1 < n_insns; i++) {
        const Instruction* push = &insns[i];
        const Instruction* pop = &insns[i + 1];

        // Must be consecutive in memory
        if (push->offset + push->length != pop->offset) continue;

        // Pop must not be a branch target
        if (branch_targets && pop->offset < text_size && branch_targets[pop->offset])
            continue;

        // Check PUSH reg (0x50-0x57) followed by POP same reg (0x58-0x5F)
        if (push->opcode[0] >= 0x50 && push->opcode[0] <= 0x57 &&
            pop->opcode[0] >= 0x58 && pop->opcode[0] <= 0x5F) {
            uint8_t push_reg = push->opcode[0] - 0x50;
            uint8_t pop_reg = pop->opcode[0] - 0x58;

            // Apply REX.B if present
            if (push->rex & 0x01) push_reg |= 8;
            if (pop->rex & 0x01) pop_reg |= 8;

            if (push_reg == pop_reg) {
                uint64_t off = push->offset;
                uint32_t total_len = push->length + pop->length;
                if (off + total_len <= text_size) {
                    peep_emit_nop(text + off, total_len);
                    eliminated++;
                    i++; // Skip the POP
                }
            }
        }
    }
    return eliminated;
}

// ============================================================================
// Pass 3: NOP sled consolidation
// Merge adjacent NOP sequences into optimal multi-byte NOPs
// ============================================================================
static uint32_t pass_consolidate_nops(uint8_t* text, uint64_t text_size,
                                       const bool* branch_targets) {
    uint32_t consolidated = 0;
    uint64_t i = 0;

    while (i < text_size) {
        uint32_t nop_len = 0;
        if (!is_nop_byte_start(text, i, text_size, &nop_len)) {
            i++;
            continue;
        }

        // Found start of a NOP — scan for the full contiguous NOP sled
        uint64_t sled_start = i;
        uint64_t sled_end = i;
        while (sled_end < text_size) {
            uint32_t nl = 0;
            if (!is_nop_byte_start(text, sled_end, text_size, &nl)) break;
            // If any byte in this NOP instruction is a branch target, stop
            bool is_target = false;
            if (branch_targets) {
                for (uint32_t k = 0; k < nl; k++) {
                    if (sled_end + k < text_size && branch_targets[sled_end + k]) {
                        is_target = true;
                        break;
                    }
                }
            }
            if (is_target && sled_end > sled_start) break; // Stop before this NOP
            sled_end += nl;
            if (is_target) break; // Include this NOP but stop after
        }

        uint64_t sled_len = sled_end - sled_start;
        if (sled_len > 1) {
            // Re-emit as optimal multi-byte NOPs
            peep_emit_nop(text + sled_start, (uint32_t)sled_len);
            consolidated++;
        }
        i = sled_end;
    }
    return consolidated;
}

// ============================================================================
// Pass 4: Dead register load elimination
// If a MOV reg, imm is followed by another MOV reg, imm to the same register
// with no intervening use, the first one is dead.
// ============================================================================
static uint32_t pass_dead_reg_load(uint8_t* text, uint64_t text_size,
                                    const Instruction* insns, uint32_t n_insns,
                                    const bool* branch_targets) {
    uint32_t eliminated = 0;
    for (uint32_t i = 0; i + 1 < n_insns; i++) {
        const Instruction* first = &insns[i];
        // Must be MOV r, imm32 (0xB8-0xBF)
        if (first->opcode[0] < 0xB8 || first->opcode[0] > 0xBF) continue;
        // Skip if first is a branch target
        if (branch_targets && first->offset < text_size && branch_targets[first->offset])
            continue;

        uint8_t first_reg = first->reg;

        // Scan forward looking for a redefinition of the same register
        // before any use of it
        for (uint32_t j = i + 1; j < n_insns && j <= i + 8; j++) {
            const Instruction* next = &insns[j];

            // If next is a branch/call/ret, stop scanning
            if (next->is_branch || next->is_call || next->is_ret) break;
            // If next is a branch target, stop
            if (branch_targets && next->offset < text_size && branch_targets[next->offset])
                break;

            // Check if next uses first_reg
            bool uses_reg = false;
            for (uint8_t k = 0; k < next->n_use_regs; k++) {
                if (next->use_regs[k] == first_reg) { uses_reg = true; break; }
            }
            if (uses_reg) break; // Register is used, first load is live

            // Check if next redefines the same register with MOV r, imm32
            if (next->opcode[0] >= 0xB8 && next->opcode[0] <= 0xBF) {
                uint8_t next_reg = next->reg;
                if (next_reg == first_reg) {
                    // First load is dead — NOP it out
                    uint64_t off = first->offset;
                    uint32_t len = first->length;
                    if (off + len <= text_size) {
                        peep_emit_nop(text + off, len);
                        eliminated++;
                    }
                    break;
                }
            }
        }
    }
    return eliminated;
}

// ============================================================================
// Pass 5: MOV reg, 0 -> XOR reg, reg
// Replaces 5-byte/6-byte MOV with 2-byte/3-byte XOR to clear a register.
// ============================================================================
static uint32_t pass_mov_zero_to_xor(uint8_t* text, uint64_t text_size,
                                     const Instruction* insns, uint32_t n_insns,
                                     const bool* branch_targets) {
    uint32_t eliminated = 0;
    for (uint32_t i = 0; i < n_insns; i++) {
        const Instruction* insn = &insns[i];
        if (branch_targets && insn->offset < text_size && branch_targets[insn->offset])
            continue;

        // Check if it is MOV reg, imm32 (0xB8-0xBF) and value is 0
        if (insn->opcode[0] >= 0xB8 && insn->opcode[0] <= 0xBF && insn->imm == 0) {
            uint8_t reg = insn->reg;
            uint64_t off = insn->offset;
            uint32_t len = insn->length;

            if (off + len <= text_size) {
                uint8_t enc[3];
                uint32_t enc_len = 0;
                if (reg < 8) {
                    enc[0] = 0x31;
                    enc[1] = 0xC0 + reg * 8 + reg;
                    enc_len = 2;
                } else {
                    enc[0] = 0x45; // REX.R and REX.B
                    enc[1] = 0x31;
                    uint8_t r = reg & 7;
                    enc[2] = 0xC0 + r * 8 + r;
                    enc_len = 3;
                }

                if (enc_len <= len) {
                    memcpy(text + off, enc, enc_len);
                    if (len > enc_len) {
                        peep_emit_nop(text + off + enc_len, len - enc_len);
                    }
                    eliminated++;
                }
            }
        }
    }
    return eliminated;
}

// ============================================================================
// Main peephole optimization entry point
// ============================================================================
int bine_peephole_optimize(uint8_t* text, uint64_t text_size, uint64_t* new_size_out) {
    if (!text || text_size == 0) {
        if (new_size_out) *new_size_out = text_size;
        return 0;
    }

    // Decode instructions for analysis using the safe decoder that
    // uses x86_64_insn_length() for correct boundaries
    uint32_t n_insns = 0;
    Instruction* insns = decode_instructions_safe(text, text_size, &n_insns);
    if (!insns || n_insns == 0) {
        if (new_size_out) *new_size_out = text_size;
        return 0;
    }

    // Build branch target set
    bool* branch_targets = build_branch_target_set(insns, n_insns, text_size);

    // Run passes
    uint32_t total_eliminated = 0;

    total_eliminated += pass_redundant_mov(text, text_size, insns, n_insns, branch_targets);
    total_eliminated += pass_push_pop_pairs(text, text_size, insns, n_insns, branch_targets);
    total_eliminated += pass_mov_zero_to_xor(text, text_size, insns, n_insns, branch_targets);
    total_eliminated += pass_dead_reg_load(text, text_size, insns, n_insns, branch_targets);

    // Always consolidate NOPs last
    pass_consolidate_nops(text, text_size, branch_targets);

    // Cleanup
    free(branch_targets);
    free_instructions(insns);

    if (new_size_out) *new_size_out = text_size;
    return 0;
}
