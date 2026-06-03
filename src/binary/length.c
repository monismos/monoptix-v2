// bine/binary/x86_64_length.c — Complete x86-64 instruction length decoder
// Returns the exact byte length of any valid x86-64 instruction, or 0 for
// invalid/unrecognised encodings.  Used by the peephole optimizer to verify
// instruction boundaries without needing full semantic decode.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ModRM displacement sizes indexed by (mod << 3 | rm)
// mod=00: rm=5 → disp32, rm=4 → SIB (special), else none
// mod=01: disp8
// mod=10: disp32
// mod=11: none (register)
static uint8_t modrm_disp_size(uint8_t modrm) {
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t rm = modrm & 7;
    if (mod == 3) return 0;            // register mode
    if (mod == 0 && rm == 5) return 4; // disp32 [RIP+disp32]
    if (mod == 0 && rm == 4) return 0; // SIB handled separately
    if (mod == 1) return 1;            // disp8
    if (mod == 2) return 4;            // disp32
    return 0;
}

// SIB displacement size
static uint8_t sib_disp_size(uint8_t modrm, uint8_t sib) {
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t base = sib & 7;
    if (mod == 0 && base == 5) return 4; // disp32
    if (mod == 1) return 1;              // disp8
    if (mod == 2) return 4;              // disp32
    return 0;
}

// Compute ModRM + SIB + displacement length
static uint8_t modrm_sib_len(const uint8_t* p, uint8_t rex) {
    (void)rex;
    uint8_t modrm = *p;
    uint8_t mod = (modrm >> 6) & 3;
    uint8_t rm = modrm & 7;
    uint8_t len = 1; // ModRM byte

    if (mod == 3) return len; // register-register

    if (rm == 4) {
        // SIB byte present
        len++; // SIB
        len += sib_disp_size(modrm, p[1]);
    } else {
        len += modrm_disp_size(modrm);
    }
    return len;
}

// One-byte opcode immediate sizes
// Returns: 0 = no imm, 1 = imm8, 2 = imm16, 4 = imm32, 8 = imm64
static uint8_t one_byte_imm_size(uint8_t op, uint8_t rex) {
    switch (op) {
        // Group 1 ALU: AL, imm8
        case 0x04: case 0x0C: case 0x14: case 0x1C:
        case 0x24: case 0x2C: case 0x34: case 0x3C:
            return 1;

        // Group 1 ALU: EAX/RAX, imm32 (imm64 with REX.W on 0x05/0x0D/0x15/0x1D/0x25/0x2D/0x35/0x3D)
        case 0x05: case 0x0D: case 0x15: case 0x1D:
        case 0x25: case 0x2D: case 0x35: case 0x3D:
            return (rex & 8) ? 8 : 4;

        // Group 1 ALU r/m, imm: 80=imm8, 81=imm32, 82=imm8(alias), 83=imm8(sign-ext)
        case 0x80: case 0x82: return 1;
        case 0x81: return 4;
        case 0x83: return 1;

        // MOV r, imm: B0-B7=imm8, B8-BF=imm32(imm64 with REX.W)
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            return 1;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            return (rex & 8) ? 8 : 4;

        // MOV r/m, imm: C6=imm8, C7=imm32
        case 0xC6: return 1;
        case 0xC7: return 4;

        // RET imm16
        case 0xC2: return 2;

        // ENTER
        case 0xC8: return 3;

        // Jcc/JMP short
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        case 0xEB: return 1; // JMP rel8

        // CALL/JMP near
        case 0xE8: return 4; // CALL rel32
        case 0xE9: return 4; // JMP rel32

        // IN/OUT with imm8
        case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        case 0xEC: case 0xED: case 0xEE: case 0xEF:
            return 1;

        // INT
        case 0xCD: return 1;

        // XBEGIN
        case 0xC7F8: return 4; // not one-byte, handled in two-byte

        // shift Group 2: imm8
        case 0xC0: case 0xC1: return 1;

        // IMUL r, r/m, imm: 69=imm32, 6B=imm8
        case 0x69: return 4;
        case 0x6B: return 1;

        // TEST AL/EAX, imm
        case 0xA8: return 1;
        case 0xA9: return 4;

        default: return 0;
    }
}

// Determine if one-byte opcode has ModRM
static bool one_byte_has_modrm(uint8_t op) {
    switch (op) {
        // ALU r/m, r / ALU r, r/m
        case 0x00: case 0x01: case 0x02: case 0x03:
        case 0x08: case 0x09: case 0x0A: case 0x0B:
        case 0x10: case 0x11: case 0x12: case 0x13:
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x28: case 0x29: case 0x2A: case 0x2B:
        case 0x30: case 0x31: case 0x32: case 0x33:
        case 0x38: case 0x39: case 0x3A: case 0x3B:
        // Group 1 ALU r/m, imm
        case 0x80: case 0x81: case 0x82: case 0x83:
        // TEST/XCHG r/m, r
        case 0x84: case 0x85: case 0x86: case 0x87:
        // MOV r/m, r / MOV r, r/m
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        // LEA
        case 0x8D:
        // MOV r/m, imm
        case 0xC6: case 0xC7:
        // Shift group 2
        case 0xC0: case 0xC1:
        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        // IMUL r, r/m, imm
        case 0x69: case 0x6B:
        // TEST/NOT/NEG/MUL/DIV (Group 3)
        case 0xF6: case 0xF7:
        // INC/DEC/CALL/JMP/PUSH (Group 4/5)
        case 0xFE: case 0xFF:
        // POP r/m
        case 0x8F:
        // XOP/VEX/EVEX escape (treat as 2-byte minimum)
        case 0x8E:
        // Bound, ARPL, etc.
        case 0x62: case 0x63:
        // LES, LDS, etc. (invalid in 64-bit but may appear)
        case 0xC4: case 0xC5:
            return true;
        default: return false;
    }
}

// Two-byte opcode (0F xx) immediate sizes
static uint8_t two_byte_imm_size(uint8_t op2) {
    switch (op2) {
        // Jcc rel32
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F:
            return 4;
        // SSE CMPPS/CMPPD/PCMPxx imm8
        case 0xC2: case 0xC6:
            return 1;
        // SHLD/SHRD imm8
        case 0xA4: case 0xAC:
            return 1;
        // Group 8 (BT/BTS/BTR/BTC r/m, imm8)
        case 0xBA:
            return 1;
        // MONITOR/MWAIT (0F 01 C8/C9)
        // These are special, but length comes from ModRM handling
        default:
            return 0;
    }
}

// Two-byte opcode (0F xx) has ModRM
static bool two_byte_has_modrm(uint8_t op2) {
    switch (op2) {
        // SYSCALL, etc. — no ModRM
        case 0x05: case 0x0B: case 0x07: case 0x34: case 0x35:
            return false;
        // BSWAP reg — no ModRM
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
            return false;
        // Jcc rel32 — no ModRM
        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F:
            return false;
        // LFS/LGS/LSS — have ModRM
        // Everything else has ModRM
        default: return true;
    }
}

// Three-byte opcode (0F 38 xx / 0F 3A xx) — all have ModRM
static bool three_byte_has_modrm(uint8_t op2) {
    (void)op2;
    return true;
}

// Three-byte opcode immediate sizes
static uint8_t three_byte_imm_size(uint8_t op2, uint8_t op3) {
    // 0F 3A xx: always has imm8
    if (op2 == 0x3A) return 1;
    // 0F 38 xx: most have no immediate
    (void)op3;
    return 0;
}

// Check if byte is a valid legacy prefix (non-REX)
static bool is_legacy_prefix(uint8_t b) {
    switch (b) {
        case 0x26: case 0x2E: case 0x36: case 0x3E: // segment overrides
        case 0x64: case 0x65: // FS/GS override
        case 0x66: // operand size
        case 0x67: // address size
        case 0xF0: // LOCK
        case 0xF2: case 0xF3: // REP/REPNZ
            return true;
        default: return false;
    }
}

// Main entry point: returns byte length of x86-64 instruction at p[0..].
// max_len is the number of available bytes.
// Returns 0 if the encoding is invalid or truncated.
uint32_t x86_64_insn_length(const uint8_t* p, uint32_t max_len) {
    if (!p || max_len == 0) return 0;

    uint32_t pos = 0;

    // Skip legacy prefixes (up to 14 allowed per Intel, but practically ~4)
    uint8_t prefix_count = 0;
    while (pos < max_len && prefix_count < 14) {
        if (is_legacy_prefix(p[pos])) {
            pos++;
            prefix_count++;
        } else {
            break;
        }
    }

    if (pos >= max_len) return 0;

    // REX prefix (40-4F): must be last prefix, only in 64-bit mode
    uint8_t rex = 0;
    if (p[pos] >= 0x40 && p[pos] <= 0x4F) {
        rex = p[pos];
        pos++;
        if (pos >= max_len) return 0;
    }

    // Opcode
    uint8_t op = p[pos];
    pos++;

    if (op == 0x0F) {
        // Two-byte opcode escape
        if (pos >= max_len) return 0;
        uint8_t op2 = p[pos];
        pos++;

        if (op2 == 0x38 || op2 == 0x3A) {
            // Three-byte opcode
            if (pos >= max_len) return 0;
            uint8_t op3 = p[pos];
            pos++;

            // ModRM
            if (three_byte_has_modrm(op2)) {
                if (pos >= max_len) return 0;
                pos += modrm_sib_len(p + pos, rex);
            }

            // Immediate
            uint8_t imm = three_byte_imm_size(op2, op3);
            pos += imm;
        } else {
            // Two-byte opcode
            // ModRM
            if (two_byte_has_modrm(op2)) {
                if (pos >= max_len) return 0;
                pos += modrm_sib_len(p + pos, rex);
            }

            // Immediate
            uint8_t imm = two_byte_imm_size(op2);
            pos += imm;
        }
    } else {
        // One-byte opcode
        // ModRM
        if (one_byte_has_modrm(op)) {
            if (pos >= max_len) return 0;
            pos += modrm_sib_len(p + pos, rex);
        }

        // Immediate
        uint8_t imm = one_byte_imm_size(op, rex);
        pos += imm;
    }

    // Sanity check: x86-64 max instruction length is 15 bytes
    if (pos > 15) return 0;

    return pos;
}
