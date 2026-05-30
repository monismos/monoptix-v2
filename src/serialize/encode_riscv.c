// bine/serialize/encode_riscv.c — RISC-V instruction encoder
// Built-in encoder for RISC-V architecture (no external assembler).

#include "bine.h"
#include <stdint.h>
#include <stdlib.h>

// RISC-V register encoding (X0-X31)
static const uint8_t riscv_reg_map[32] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
};

// Encode ADDI (add immediate)
static uint32_t encode_addi(uint8_t rd, uint8_t rs1, int16_t imm) {
    // ADDI rd, rs1, imm: imm[11:0] | rs1[4:0] | 000 | rd[4:0] | 0010011
    uint32_t imm_12 = (imm & 0xFFF) << 20;
    uint32_t rs1_field = (rs1 & 0x1F) << 15;
    uint32_t rd_field = (rd & 0x1F) << 7;
    return imm_12 | rs1_field | rd_field | 0x0010011;
}

// Encode MV (register to register using ADDI with imm=0)
static uint32_t encode_mv(uint8_t rd, uint8_t rs1) {
    return encode_addi(rd, rs1, 0);
}

// Encode LUI (load upper immediate)
static uint32_t encode_lui(uint8_t rd, uint32_t imm) {
    // LUI rd, imm: imm[31:12] | rd[4:0] | 0110111
    uint32_t imm_20 = ((imm >> 12) & 0xFFFFF) << 12;
    uint32_t rd_field = (rd & 0x1F) << 7;
    return imm_20 | rd_field | 0x0110111;
}

// Encode JALR (jump and link register)
static uint32_t encode_jalr(uint8_t rd, uint8_t rs1, int16_t imm) {
    // JALR rd, rs1, imm: imm[11:0] | rs1[4:0] | 000 | rd[4:0] | 1100111
    uint32_t imm_12 = (imm & 0xFFF) << 20;
    uint32_t rs1_field = (rs1 & 0x1F) << 15;
    uint32_t rd_field = (rd & 0x1F) << 7;
    return imm_12 | rs1_field | rd_field | 0x1100111;
}

// Encode RET (JALR X0, X1, 0)
static uint32_t encode_ret() {
    return encode_jalr(0, 1, 0);
}

// Encode NOP (ADDI X0, X0, 0)
static uint32_t encode_nop() {
    return encode_addi(0, 0, 0);
}

// Encode agent to RISC-V instruction
uint32_t encode_agent_riscv(uint64_t agent, uint32_t reg_assign,
                             uint8_t* out) {
    AgentPacked ap = *(AgentPacked*)&agent;
    uint32_t insn;
    
    switch (ap.type) {
        case AGENT_CON:
            if (ap.port_0 < 65536) {
                // Immediate value - use LUI + ADDI
                insn = encode_lui(reg_assign & 0x1F, ap.port_0);
            } else {
                // Register to register
                insn = encode_mv(reg_assign & 0x1F, (ap.port_0 >> 8) & 0x1F);
            }
            break;
        
        case AGENT_DUP:
            insn = encode_mv(reg_assign & 0x1F, (ap.port_0 >> 8) & 0x1F);
            break;
        
        case AGENT_ERA:
            insn = encode_nop();
            break;
        
        case AGENT_SIFT:
            insn = encode_lui(reg_assign & 0x1F, ap.port_0);
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
