// bine/serialize/encode_aarch64.c — AArch64 instruction encoder
// Built-in encoder for AArch64 architecture (no external assembler).

#include "bine.h"
#include <stdint.h>
#include <stdlib.h>

// AArch64 register encoding (X0-X30)
static const uint8_t a64_reg_map[31] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30
};

// Encode MOV (register to register)
static uint32_t encode_mov_reg_reg(uint8_t dst_reg, uint8_t src_reg) {
    // MOV Xd, Xn: 0xAA0003E0 | (dst << 0) | (src << 5)
    return 0xAA0003E0 | (dst_reg << 0) | (src_reg << 5);
}

// Encode MOV (wide immediate)
static uint32_t encode_mov_imm(uint16_t imm, uint8_t dst_reg) {
    // MOVZ Xd, #imm: 0x52800000 | (imm << 5) | dst
    return 0x52800000 | (imm << 5) | dst_reg;
}

// Encode RET
static uint32_t encode_ret() {
    return 0xD65F03C0;
}

// Encode NOP
static uint32_t encode_nop() {
    return 0xD503201F;
}

// Encode agent to AArch64 instruction
uint32_t encode_agent_aarch64(uint64_t agent, uint32_t reg_assign,
                               uint8_t* out) {
    AgentPacked ap = *(AgentPacked*)&agent;
    uint32_t insn;
    
    switch (ap.type) {
        case AGENT_CON:
            if (ap.port_0 < 65536) {
                // Immediate value (16-bit)
                insn = encode_mov_imm(ap.port_0 & 0xFFFF, reg_assign & 0x1F);
            } else {
                // Register to register
                insn = encode_mov_reg_reg(reg_assign & 0x1F, (ap.port_0 >> 8) & 0x1F);
            }
            break;
        
        case AGENT_DUP:
            insn = encode_mov_reg_reg(reg_assign & 0x1F, (ap.port_0 >> 8) & 0x1F);
            break;
        
        case AGENT_ERA:
            insn = encode_nop();
            break;
        
        case AGENT_SIFT:
            insn = encode_mov_imm(ap.port_0 & 0xFFFF, reg_assign & 0x1F);
            break;
        
        case AGENT_ATOMIC:
            insn = encode_nop();
            break;
        
        default:
            insn = encode_nop();
            break;
    }
    
    // Write instruction in little-endian
    out[0] = insn & 0xFF;
    out[1] = (insn >> 8) & 0xFF;
    out[2] = (insn >> 16) & 0xFF;
    out[3] = (insn >> 24) & 0xFF;
    
    return 4;
}
