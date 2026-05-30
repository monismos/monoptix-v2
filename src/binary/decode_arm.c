// bine/parse/aarch64_decode.c — AArch64 instruction decoder
// Decodes ARM64 instructions for BINE optimization pipeline

#include "bine.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// AArch64 Instruction Decoding
// ============================================================================

// Decode a single AArch64 instruction
int aarch64_decode(const uint8_t* code, uint64_t offset, uint64_t max_len,
                   Instruction* insn) {
    if (!code || !insn) return -1;
    if (offset + 4 > max_len) return -1;
    
    memset(insn, 0, sizeof(Instruction));
    
    // AArch64 instructions are always 4 bytes (32-bit)
    insn->offset = offset;
    insn->length = 4;
    
    // Read instruction word (little-endian)
    uint32_t word = *(uint32_t*)(code + offset);
    
    // Copy opcode bytes
    memcpy(insn->opcode, code + offset, 4);
    
    // Decode instruction class and fields
    uint32_t op0 = (word >> 25) & 0xF;  // Bits 25-28
    uint32_t op1 = (word >> 23) & 0x3;  // Bits 23-24
    uint32_t op2 = (word >> 16) & 0x3F; // Bits 16-21
    uint32_t rd   = (word >> 0)  & 0x1F; // Bits 0-4 (destination register)
    uint32_t rn   = (word >> 5)  & 0x1F; // Bits 5-9 (source register)
    uint32_t rm   = (word >> 16) & 0x1F; // Bits 16-20 (source register)
    
    // Store register information
    insn->reg = rd;
    
    // Identify instruction types
    // Branch instructions (B, BL, BR, BLR, RET)
    if (op0 == 0x5 || op0 == 0x6 || op0 == 0x7) {
        uint32_t opc = (word >> 31) & 0x1;
        if (opc == 0 && op0 == 0x5) {
            // Unconditional branch (B, BL)
            insn->is_branch = true;
            int32_t imm26 = (int32_t)((word & 0x3FFFFFF) << 2) >> 2;
            insn->target = offset + imm26;
        } else if (opc == 1 && op0 == 0x5) {
            // Unconditional branch with link (BL)
            insn->is_call = true;
            int32_t imm26 = (int32_t)((word & 0x3FFFFFF) << 2) >> 2;
            insn->target = offset + imm26;
        } else if (opc == 0 && (op0 == 0x6 || op0 == 0x7)) {
            // Unconditional branch to register (BR, BLR)
            insn->is_branch = true;
            // Target is in register, not immediate
        } else if (opc == 1 && op0 == 0x6) {
            // Return from subroutine (RET)
            insn->is_ret = true;
        }
    }
    
    // Conditional branch (B.cond)
    if (op0 == 0x4) {
        insn->is_branch = true;
        int32_t imm19 = (int32_t)((word & 0x7FFFF) << 2) >> 2;
        insn->target = offset + imm19;
    }
    
    // Load/Store instructions (LDR, STR, etc.)
    if (op0 >= 0x4 && op0 <= 0x7) {
        uint32_t op2_val = (word >> 10) & 0x3;
        if (op2_val == 0x0 || op2_val == 0x2) {
            insn->has_modrm = true;
            insn->modrm = word & 0xFFFF;  // Store addressing mode
        }
    }
    
    // Load literal (LDR)
    if (op0 == 0x0 && op1 == 0x0 && op2 == 0x0) {
        insn->has_modrm = true;
        int32_t imm19 = (int32_t)((word & 0x7FFFF) << 2) >> 2;
        insn->target = offset + imm19;
    }
    
    return 4;
}

// Decode all AArch64 instructions in a code segment
Instruction* aarch64_decode_all(const uint8_t* code, uint64_t code_size,
                                 uint32_t* n_insns) {
    if (!code || code_size == 0) return NULL;
    
    // Estimate number of instructions (4 bytes each)
    uint32_t estimated = (code_size + 3) / 4;
    Instruction* insns = (Instruction*)calloc(estimated, sizeof(Instruction));
    if (!insns) return NULL;
    
    uint32_t count = 0;
    uint64_t offset = 0;
    
    while (offset + 4 <= code_size) {
        int len = aarch64_decode(code, offset, code_size, &insns[count]);
        if (len < 0) {
            // Failed to decode - skip this byte
            offset++;
            continue;
        }
        
        offset += len;
        count++;
        
        if (count >= estimated) {
            // Need more space
            estimated *= 2;
            Instruction* new_insns = (Instruction*)realloc(insns, estimated * sizeof(Instruction));
            if (!new_insns) {
                free(insns);
                return NULL;
            }
            insns = new_insns;
        }
    }
    
    *n_insns = count;
    
    // Shrink to actual size
    if (count > 0) {
        Instruction* final_insns = (Instruction*)realloc(insns, count * sizeof(Instruction));
        if (final_insns) insns = final_insns;
    }
    
    return insns;
}
