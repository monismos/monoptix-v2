// bine/serialize/encode_x86_64.c — x86-64 instruction encoder
// Built-in encoder for x86-64 architecture (no external assembler).

#include "bine.h"
#include <stdint.h>
#include <stdlib.h>

// x86-64 register encoding
static const uint8_t x64_reg_map[16] = {
    0,  // RAX
    1,  // RCX
    2,  // RDX
    3,  // RBX
    4,  // RSP
    5,  // RBP
    6,  // RSI
    7,  // RDI
    8,  // R8
    9,  // R9
    10, // R10
    11, // R11
    12, // R12
    13, // R13
    14, // R14
    15  // R15
};

// Encode a MOV instruction (register to register)
static uint32_t encode_mov_reg_reg(uint8_t dst_reg, uint8_t src_reg, uint8_t* out) {
    // REX.W prefix for 64-bit operands
    out[0] = 0x48; // REX.W
    
    // ModRM: reg=dst, rm=src
    out[1] = 0x88 | ((dst_reg & 7) << 3) | (src_reg & 7);
    
    return 2;
}

// Encode a MOV instruction (immediate to register)
static uint32_t encode_mov_imm_reg(uint32_t imm, uint8_t dst_reg, uint8_t* out) {
    // MOV r/m32, imm32
    out[0] = 0xB8 + (dst_reg & 7);
    
    // Immediate (little-endian)
    out[1] = imm & 0xFF;
    out[2] = (imm >> 8) & 0xFF;
    out[3] = (imm >> 16) & 0xFF;
    out[4] = (imm >> 24) & 0xFF;
    
    return 5;
}

// Encode a RET instruction
static uint32_t encode_ret(uint8_t* out) {
    out[0] = 0xC3;
    return 1;
}

// Encode agent to x86-64 instruction
uint32_t encode_agent_x86_64(uint64_t agent, uint32_t reg_assign,
                              uint8_t* out) {
    AgentPacked ap = *(AgentPacked*)&agent;
    
    switch (ap.type) {
        case AGENT_CON:
            // Single-use computation → MOV or similar
            if (ap.port_0 < 256) {
                // Immediate value
                return encode_mov_imm_reg(ap.port_0, reg_assign & 0xF, out);
            } else {
                // Register to register
                return encode_mov_reg_reg(reg_assign & 0xF, (ap.port_0 >> 8) & 0xF, out);
            }
        
        case AGENT_DUP:
            // Shared computation → same as CON for now
            return encode_mov_reg_reg(reg_assign & 0xF, (ap.port_0 >> 8) & 0xF, out);
        
        case AGENT_ERA:
            // Dead computation → NOP (0x90)
            out[0] = 0x90;
            return 1;
        
        case AGENT_SIFT:
            // Constant/leaf value
            return encode_mov_imm_reg(ap.port_0, reg_assign & 0xF, out);
        
        case AGENT_ATOMIC:
            // Opaque region → NOP (would preserve original bytes)
            out[0] = 0x90;
            return 1;
        
        default:
            out[0] = 0x90; // NOP as fallback
            return 1;
    }
}
