// bine/parse/x86_64_decode.c — x86-64 instruction decoder
// Decodes x86-64 instructions for CFG/DFG construction without external libraries.

#include "bine.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Get REX prefix fields
static uint8_t rex_w(uint8_t rex) { return (rex >> 3) & 1; }
static uint8_t rex_r(uint8_t rex) { return (rex >> 2) & 1; }
static uint8_t rex_x(uint8_t rex) { return (rex >> 1) & 1; }
static uint8_t rex_b(uint8_t rex) { return rex & 1; }

// Get ModRM fields
static uint8_t modrm_mod(uint8_t modrm) { return (modrm >> 6) & 3; }
static uint8_t modrm_reg(uint8_t modrm) { return (modrm >> 3) & 7; }
static uint8_t modrm_rm(uint8_t modrm) { return modrm & 7; }

// Decode ModRM with SIB
static uint32_t decode_modrm_sib(const uint8_t* bytes, uint32_t offset, uint8_t rex,
                                  uint8_t* reg_out, uint8_t* rm_out, uint8_t* disp_size) {
    uint8_t modrm = bytes[offset];
    uint8_t mod = modrm_mod(modrm);
    uint8_t reg = modrm_reg(modrm);
    uint8_t rm = modrm_rm(modrm);
    
    // Apply REX extensions
    if (rex_r(rex)) reg |= 8;
    if (rex_b(rex)) rm |= 8;
    
    *reg_out = reg;
    *rm_out = rm;
    *disp_size = 0;
    
    uint32_t total = 1; // ModRM byte
    
    // Check for SIB byte
    if (rm == 4 && mod != 3) {
        uint8_t sib = bytes[offset + 1];
        total++; // SIB byte
        
        // SIB base
        uint8_t base = sib & 7;
        if (rex_b(rex)) base |= 8;
        
        // Disp based on mod and base
        if (mod == 0 && base == 5) {
            *disp_size = 4; // disp32
            total += 4;
        } else if (mod == 1) {
            *disp_size = 1; // disp8
            total += 1;
        } else if (mod == 2) {
            *disp_size = 4; // disp32
            total += 4;
        }
    } else {
        // No SIB
        if (mod == 0 && rm == 5) {
            *disp_size = 4; // disp32 (absolute address)
            total += 4;
        } else if (mod == 1) {
            *disp_size = 1; // disp8
            total += 1;
        } else if (mod == 2) {
            *disp_size = 4; // disp32
            total += 4;
        }
    }
    
    return total;
}

// Decode single instruction
static uint32_t decode_instruction(const uint8_t* bytes, uint64_t offset, uint64_t max_len,
                                   Instruction* insn) {
    if (offset >= max_len) return 0;
    
    memset(insn, 0, sizeof(Instruction));
    insn->offset = offset;
    
    uint64_t pos = offset;
    uint8_t rex = 0;
    
    // Check for REX prefix
    if (bytes[pos] >= 0x40 && bytes[pos] <= 0x4F) {
        rex = bytes[pos];
        pos++;
        if (pos >= max_len) return 0;
    }
    
    insn->rex = rex;
    uint8_t opcode = bytes[pos];
    insn->opcode[0] = opcode;
    pos++;
    
    uint32_t len = 1;
    if (rex != 0) len++;
    
    // Decode based on opcode
    switch (opcode) {
        case 0x50: case 0x51: case 0x52: case 0x53: // PUSH reg
        case 0x54: case 0x55: case 0x56: case 0x57:
            insn->reg = (opcode - 0x50) & 7;
            if (rex_b(rex)) insn->reg |= 8;
            break;
        
        case 0x58: case 0x59: case 0x5A: case 0x5B: // POP reg
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            insn->reg = (opcode - 0x58) & 7;
            if (rex_b(rex)) insn->reg |= 8;
            break;
        
        case 0x01: // ADD r/m, r
        case 0x09: // OR r/m, r
        case 0x11: // ADC r/m, r
        case 0x19: // SBB r/m, r
        case 0x21: // AND r/m, r
        case 0x29: // SUB r/m, r
        case 0x31: // XOR r/m, r
            insn->has_modrm = true;
            {
                uint8_t reg_out, rm_out, disp_size;
                uint32_t modrm_len = decode_modrm_sib(bytes, pos, rex, &reg_out, &rm_out, &disp_size);
                insn->modrm = bytes[pos];
                insn->use_regs[insn->n_use_regs++] = reg_out; // Source register
                insn->rm = rm_out; // Destination (might be memory or register)
                len += modrm_len + disp_size;
            }
            break;
        
        case 0x03: // ADD r, r/m
        case 0x0B: // OR r, r/m
        case 0x13: // ADC r, r/m
        case 0x1B: // SBB r, r/m
        case 0x23: // AND r, r/m
        case 0x2B: // SUB r, r/m
        case 0x33: // XOR r, r/m
            insn->has_modrm = true;
            {
                uint8_t reg_out, rm_out, disp_size;
                uint32_t modrm_len = decode_modrm_sib(bytes, pos, rex, &reg_out, &rm_out, &disp_size);
                insn->modrm = bytes[pos];
                insn->reg = reg_out; // Destination register
                insn->use_regs[insn->n_use_regs++] = rm_out; // Source register
                len += modrm_len + disp_size;
            }
            break;
        
        case 0x89: // MOV r/m, r
            insn->has_modrm = true;
            {
                uint8_t reg_out, rm_out, disp_size;
                uint32_t modrm_len = decode_modrm_sib(bytes, pos, rex, &reg_out, &rm_out, &disp_size);
                insn->modrm = bytes[pos];
                insn->use_regs[insn->n_use_regs++] = reg_out; // Source register
                insn->rm = rm_out; // Destination (might be memory or register)
                len += modrm_len + disp_size;
            }
            break;
        
        case 0x8B: // MOV r, r/m
            insn->has_modrm = true;
            {
                uint8_t reg_out, rm_out, disp_size;
                uint32_t modrm_len = decode_modrm_sib(bytes, pos, rex, &reg_out, &rm_out, &disp_size);
                insn->modrm = bytes[pos];
                insn->reg = reg_out; // Destination register
                insn->use_regs[insn->n_use_regs++] = rm_out; // Source register
                len += modrm_len + disp_size;
            }
            break;
        
        case 0x39: // CMP r/m, r
        case 0x3B: // CMP r, r/m
            insn->has_modrm = true;
            {
                uint8_t reg_out, rm_out, disp_size;
                uint32_t modrm_len = decode_modrm_sib(bytes, pos, rex, &reg_out, &rm_out, &disp_size);
                insn->modrm = bytes[pos];
                insn->use_regs[insn->n_use_regs++] = reg_out; // Source register
                insn->use_regs[insn->n_use_regs++] = rm_out; // Destination register
                len += modrm_len + disp_size;
            }
            break;
        
        case 0x85: // TEST r/m, r
            insn->has_modrm = true;
            {
                uint8_t reg_out, rm_out, disp_size;
                uint32_t modrm_len = decode_modrm_sib(bytes, pos, rex, &reg_out, &rm_out, &disp_size);
                insn->modrm = bytes[pos];
                insn->use_regs[insn->n_use_regs++] = reg_out; // Source register
                insn->use_regs[insn->n_use_regs++] = rm_out; // Destination register
                len += modrm_len + disp_size;
            }
            break;
        
        case 0x8D: // LEA r, m
            insn->has_modrm = true;
            {
                uint8_t reg_out, rm_out, disp_size;
                uint32_t modrm_len = decode_modrm_sib(bytes, pos, rex, &reg_out, &rm_out, &disp_size);
                insn->modrm = bytes[pos];
                insn->reg = reg_out; // Destination register
                len += modrm_len + disp_size;
            }
            break;
        
        case 0xFF: // Group extensions (INC, DEC, CALL, JMP)
            insn->has_modrm = true;
            {
                uint8_t reg_out, rm_out, disp_size;
                uint32_t modrm_len = decode_modrm_sib(bytes, pos, rex, &reg_out, &rm_out, &disp_size);
                insn->modrm = bytes[pos];
                uint8_t reg_field = modrm_reg(insn->modrm);
                if (reg_field == 0) { // INC
                    insn->rm = rm_out; // Destination
                } else if (reg_field == 1) { // DEC
                    insn->rm = rm_out; // Destination
                }
                len += modrm_len + disp_size;
            }
            break;
        
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: // MOV r, imm32
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            insn->reg = (opcode - 0xB8) & 7;
            if (rex_b(rex)) insn->reg |= 8;
            if (pos + 4 <= max_len) {
                insn->imm = *(int32_t*)(bytes + pos);
                len += 4;
            }
            break;
        
        case 0xE8: // CALL rel32
            insn->is_call = true;
            if (pos + 4 <= max_len) {
                insn->imm = *(int32_t*)(bytes + pos);
                insn->target = offset + len + 4 + insn->imm;
                len += 4;
            }
            break;
        
        case 0xE9: // JMP rel32
            insn->is_branch = true;
            if (pos + 4 <= max_len) {
                insn->imm = *(int32_t*)(bytes + pos);
                insn->target = offset + len + 4 + insn->imm;
                len += 4;
            }
            break;
        
        case 0xEB: // JMP rel8
            insn->is_branch = true;
            if (pos + 1 <= max_len) {
                insn->imm = (int8_t)bytes[pos];
                insn->target = offset + len + 1 + insn->imm;
                len += 1;
            }
            break;
        
        case 0xC3: // RET
            insn->is_ret = true;
            break;
        
        case 0xC2: // RET imm16
            insn->is_ret = true;
            if (pos + 2 <= max_len) {
                len += 2;
            }
            break;
        
        case 0x70: case 0x71: case 0x72: case 0x73: // Jcc rel8
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            insn->is_branch = true;
            if (pos + 1 <= max_len) {
                insn->imm = (int8_t)bytes[pos];
                insn->target = offset + len + 1 + insn->imm;
                len += 1;
            }
            break;
        
        case 0x0F: // Two-byte opcode prefix
            if (pos + 1 < max_len) {
                uint8_t opcode2 = bytes[pos];
                insn->opcode[1] = opcode2;
                pos++;
                len++;
                
                switch (opcode2) {
                    case 0x1F: // Multi-byte NOP (0F 1F xx)
                        insn->has_modrm = true;
                        {
                            uint8_t reg_out, rm_out, disp_size;
                            uint32_t modrm_len = decode_modrm_sib(bytes, pos, rex, &reg_out, &rm_out, &disp_size);
                            insn->modrm = bytes[pos];
                            len += modrm_len + disp_size;
                        }
                        break;
                    
                    case 0x80: case 0x81: case 0x82: case 0x83: // Jcc rel32
                    case 0x84: case 0x85: case 0x86: case 0x87:
                    case 0x88: case 0x89: case 0x8A: case 0x8B:
                    case 0x8C: case 0x8D: case 0x8E: case 0x8F:
                        insn->is_branch = true;
                        if (pos + 4 <= max_len) {
                            insn->imm = *(int32_t*)(bytes + pos);
                            insn->target = offset + len + 4 + insn->imm;
                            len += 4;
                        }
                        break;
                    
                    case 0xB0: case 0xB1: case 0xB2: case 0xB3: // MOVZX
                    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
                        insn->has_modrm = true;
                        {
                            uint8_t reg_out, rm_out, disp_size;
                            uint32_t modrm_len = decode_modrm_sib(bytes, pos, rex, &reg_out, &rm_out, &disp_size);
                            insn->modrm = bytes[pos];
                            insn->reg = reg_out;
                            insn->rm = rm_out;
                            len += modrm_len + disp_size;
                        }
                        break;
                    
                    case 0xAF: // IMUL r, r/m
                        insn->has_modrm = true;
                        {
                            uint8_t reg_out, rm_out, disp_size;
                            uint32_t modrm_len = decode_modrm_sib(bytes, pos, rex, &reg_out, &rm_out, &disp_size);
                            insn->modrm = bytes[pos];
                            insn->reg = reg_out;
                            insn->rm = rm_out;
                            len += modrm_len + disp_size;
                        }
                        break;
                    
                    default: // Most two-byte opcodes have ModRM
                        insn->has_modrm = true;
                        {
                            uint8_t reg_out, rm_out, disp_size;
                            uint32_t modrm_len = decode_modrm_sib(bytes, pos, rex, &reg_out, &rm_out, &disp_size);
                            insn->modrm = bytes[pos];
                            len += modrm_len + disp_size;
                        }
                        break;
                }
            }
            break;
        
        default:
            // For unknown opcodes, attempt to infer length from encoding
            // Check for ModRM byte (most instructions have ModRM)
            if (pos + 1 < max_len) {
                uint8_t next_byte = bytes[pos];
                uint8_t mod = (next_byte >> 6) & 3;
                // Check if it looks like a ModRM byte (mod field is valid)
                if (mod <= 2) {
                    insn->has_modrm = true;
                    insn->modrm = next_byte;
                    len += 1; // ModRM
                    // Check for SIB
                    uint8_t rm = next_byte & 7;
                    if (rm == 4 && mod != 3) {
                        len += 1; // SIB
                    }
                    // Add displacement based on mod
                    if (mod == 1) len += 1; // disp8
                    else if (mod == 2 || (mod == 0 && rm == 5)) len += 4; // disp32
                } else {
                    // mod == 3 (register-only) might still be ModRM
                    // Check if this is a common instruction with ModRM
                    if (opcode >= 0x00 && opcode <= 0xFF) {
                        insn->has_modrm = true;
                        insn->modrm = next_byte;
                        len += 1; // ModRM
                    }
                }
            }
            break;
    }
    
    insn->length = len;
    return len;
}

// Decode all instructions in a byte array
Instruction* decode_instructions(const uint8_t* text, uint64_t text_size, uint32_t* n_insns) {
    if (!text || text_size == 0) {
        *n_insns = 0;
        return NULL;
    }

    if (text_size > UINT32_MAX) {
        *n_insns = 0;
        return NULL;
    }

    // Worst case is one byte per instruction. The previous text_size / 15 cap
    // stopped decoding early on real binaries and made later stages serialize
    // only a prefix of .text.
    uint32_t max_insns = (uint32_t)text_size;
    Instruction* insns = (Instruction*)calloc(max_insns, sizeof(Instruction));
    if (!insns) {
        *n_insns = 0;
        return NULL;
    }

    uint32_t count = 0;
    uint64_t offset = 0;

    while (offset < text_size && count < max_insns) {
        Instruction insn;
        uint32_t len = decode_instruction(text, offset, text_size, &insn);
        if (len == 0) break;

        insns[count] = insn;
        count++;
        offset += len;
    }

    // Reallocate to exact size
    if (count > 0 && count < max_insns) {
        Instruction* tmp = (Instruction*)realloc(insns, count * sizeof(Instruction));
        if (tmp) insns = tmp;
    }

    *n_insns = count;
    return insns;
}


void free_instructions(Instruction* insns) {
    free(insns);
}
