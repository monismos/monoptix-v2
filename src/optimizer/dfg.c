/*
 * dfg.c - Data-Flow Graph construction from x86-64
 *
 * Replaces binary_to_icor.c.  Each instruction becomes one or more
 * ICOR nodes with explicit wire connections via def-use chains.
 */

#include "dfg.h"
#include "bbf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int dfg_program_init(dfg_program_t *p) {
    if (!p) return -1;
    p->items = NULL;
    p->n = 0;
    return 0;
}

void dfg_program_free(dfg_program_t *p) {
    if (!p) return;
    free(p->items);
    p->items = NULL;
    p->n = 0;
}

/* ---------- Instruction length decoder (slim inline) ----------
 * Full version lives in src/binary/length.c.  This is a minimal copy. */
static int x86_64_len(const uint8_t *p, int maxlen) {
    int i = 0;
    while (i < maxlen) {
        uint8_t b = p[i];
        if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            (b >= 0x40 && b <= 0x4F)) { i++; continue; }
        /* Opcode */
        i++;
        if (b == 0x0F) {
            if (i >= maxlen) return i;
            i++;  /* second opcode byte */
        }
        /* ModRM */
        if (i < maxlen) {
            uint8_t mod = (p[i] >> 6) & 3;
            uint8_t rm  = p[i] & 7;
            uint8_t modrm_op = b == 0x0F ? p[i-1] : b;
            (void)modrm_op;
            i++;
            /* SIB */
            if (mod != 3 && rm == 4 && i < maxlen) i++;
            /* Displacement */
            if (mod == 1) i += 1;
            else if (mod == 2) i += 4;
            else if (mod == 0 && rm == 5) i += 4;
        }
        /* Immediate (rough heuristic) */
        if (b == 0xB8 || (b >= 0xB0 && b <= 0xB7) || b == 0x04 || b == 0x0C) {
            /* MOV r, imm */
            if (b == 0xB8) i += 8;
            else i += 1;
        } else if (b == 0xC7) {
            i += 4;
        } else if (b == 0xE8 || b == 0xE9) {
            i += 4;
        } else if (b == 0xEB) {
            i += 1;
        } else if (b == 0x0F) {
            /* Two-byte opcode: usually Jcc short with 4-byte disp */
            i += 4;
        }
        return i;
    }
    return i;
}

/* ---------- Decoder ----------
 * Heuristic decoder: peek at first 1-3 bytes and pick a kind.  We do not
 * aim for perfection; we aim to identify the 30 ops in dfg.h. */
int dfg_decode(const uint8_t *text, size_t len, uint64_t base_vaddr,
               dfg_program_t *out) {
    if (!text || !out) return -1;
    dfg_program_init(out);
    /* First pass: count instructions */
    size_t cap = 0, n = 0;
    dfg_instr_t *items = NULL;
    size_t pos = 0;
    while (pos < len) {
        int ilen = x86_64_len(text + pos, (int)(len - pos));
        if (ilen <= 0) break;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            items = (dfg_instr_t*)realloc(items, cap * sizeof(dfg_instr_t));
            if (!items) return -2;
        }
        dfg_instr_t *ins = &items[n++];
        memset(ins, 0, sizeof(*ins));
        ins->dst_reg = 0xFF;
        ins->src_reg = 0xFF;
        ins->src2_reg = 0xFF;
        ins->address = base_vaddr + pos;
        /* Skip REX prefixes (0x40..0x4F) and other prefixes to find
         * the actual opcode byte for classification. */
        size_t op_pos = pos;
        while (op_pos < len) {
            uint8_t pb = text[op_pos];
            int is_prefix = (pb == 0x66 || pb == 0x67 || pb == 0xF0 ||
                             pb == 0xF2 || pb == 0xF3 ||
                             (pb >= 0x40 && pb <= 0x4F));
            if (!is_prefix) break;
            op_pos++;
        }
        /* Heuristic classification by opcode byte */
        uint8_t b0 = (op_pos < len) ? text[op_pos] : 0;
        uint8_t b1 = (op_pos + 1 < len) ? text[op_pos + 1] : 0;
        uint8_t modrm = (op_pos + 1 < len) ? text[op_pos + 1] : 0;
        /* Adjust the length decoder call to start at the opcode byte
         * so we get the right total length including the prefix. */
        int ilen_adj = x86_64_len(text + pos, (int)(len - pos));
        (void)ilen_adj;
        switch (b0) {
            case 0x90: ins->kind = DI_NOP; break;
            case 0xC3: ins->kind = DI_RET; break;
            case 0xE8: ins->kind = DI_CALL; ins->imm = (int32_t)(text[pos+1] | (text[pos+2]<<8) | (text[pos+3]<<16) | (text[pos+4]<<24)); break;
            case 0xE9: ins->kind = DI_JMP;  ins->imm = (int32_t)(text[pos+1] | (text[pos+2]<<8) | (text[pos+3]<<16) | (text[pos+4]<<24)); break;
            case 0xEB: ins->kind = DI_JMP;  ins->imm = (int8_t)text[pos+1]; break;
            case 0x0F: {
                if (b1 == 0x84) { ins->kind = DI_JZ;  ins->imm = (int32_t)(text[pos+2] | (text[pos+3]<<8) | (text[pos+4]<<16) | (text[pos+5]<<24)); }
                else if (b1 == 0x85) { ins->kind = DI_JNZ; ins->imm = (int32_t)(text[pos+2] | (text[pos+3]<<8) | (text[pos+4]<<16) | (text[pos+5]<<24)); }
                else if (b1 == 0xAF) { ins->kind = DI_IMUL; ins->dst_reg = text[op_pos+2] & 7; ins->src_reg = (text[op_pos+2] >> 3) & 7; }
                else { ins->kind = DI_UNKNOWN; }
                break;
            }
            case 0x89: ins->kind = DI_MOV_RR; ins->dst_reg = modrm & 7; ins->src_reg = (modrm >> 3) & 7; break;
            case 0x8B: ins->kind = DI_MOV_RR; ins->dst_reg = (modrm >> 3) & 7; ins->src_reg = modrm & 7; break;
            case 0x01: ins->kind = DI_ADD_RR; ins->dst_reg = modrm & 7; ins->src_reg = (modrm >> 3) & 7; ins->read_dst = 1; break;
            case 0x03: ins->kind = DI_ADD_RR; ins->dst_reg = (modrm >> 3) & 7; ins->src_reg = modrm & 7; ins->read_dst = 1; break;
            case 0x29: ins->kind = DI_SUB_RR; ins->dst_reg = modrm & 7; ins->src_reg = (modrm >> 3) & 7; ins->read_dst = 1; break;
            case 0x2B: ins->kind = DI_SUB_RR; ins->dst_reg = (modrm >> 3) & 7; ins->src_reg = modrm & 7; ins->read_dst = 1; break;
            case 0x21: ins->kind = DI_AND_RR; ins->dst_reg = modrm & 7; ins->src_reg = (modrm >> 3) & 7; ins->read_dst = 1; break;
            case 0x09: ins->kind = DI_OR_RR;  ins->dst_reg = modrm & 7; ins->src_reg = (modrm >> 3) & 7; ins->read_dst = 1; break;
            case 0x31: ins->kind = DI_XOR_RR; ins->dst_reg = modrm & 7; ins->src_reg = (modrm >> 3) & 7; ins->read_dst = 1; break;
            case 0x39: ins->kind = DI_CMP_RR; ins->dst_reg = modrm & 7; ins->src_reg = (modrm >> 3) & 7; break;
            case 0x3B: ins->kind = DI_CMP_RR; ins->dst_reg = (modrm >> 3) & 7; ins->src_reg = modrm & 7; break;
            case 0x85: ins->kind = DI_TEST_RR; ins->dst_reg = modrm & 7; ins->src_reg = (modrm >> 3) & 7; break;
            case 0x50: case 0x51: case 0x52: case 0x53:
            case 0x54: case 0x55: case 0x56: case 0x57: ins->kind = DI_PUSH_R; ins->src_reg = b0 & 7; break;
            case 0x58: case 0x59: case 0x5A: case 0x5B:
            case 0x5C: case 0x5D: case 0x5E: case 0x5F: ins->kind = DI_POP_R;  ins->dst_reg = b0 & 7; break;
            case 0xB8: case 0xB9: case 0xBA: case 0xBB:
            case 0xBC: case 0xBD: case 0xBE: case 0xBF: ins->kind = DI_MOV_RI; ins->dst_reg = b0 & 7; ins->imm = (int32_t)(text[op_pos+1] | (text[op_pos+2]<<8) | (text[op_pos+3]<<16) | (text[op_pos+4]<<24)); break;
            case 0xC7: ins->kind = DI_MOV_RI; ins->dst_reg = modrm & 7; ins->imm = (int32_t)(text[op_pos+2] | (text[op_pos+3]<<8) | (text[op_pos+4]<<16) | (text[op_pos+5]<<24)); break;
            case 0x81: {
                /* Group 1: ADD/SUB/CMP r/m, imm32 */
                int op = (modrm >> 3) & 7;
                if (op == 0)      ins->kind = DI_ADD_RI;
                else if (op == 5) ins->kind = DI_SUB_RI;
                else if (op == 7) ins->kind = DI_CMP_RI;
                else              ins->kind = DI_UNKNOWN;
                ins->dst_reg = modrm & 7;
                ins->read_dst = 1;
                ins->imm = (int32_t)(text[op_pos+2] | (text[op_pos+3]<<8) | (text[op_pos+4]<<16) | (text[op_pos+5]<<24));
                break;
            }
            default: ins->kind = DI_UNKNOWN; break;
        }
        pos += ilen;
    }
    out->items = items;
    out->n = n;
    return 0;
}

/* Map dfg_instr_kind_t -> BBF node type */
static uint8_t kind_to_bnt(dfg_instr_kind_t k) {
    switch (k) {
        case DI_MOV_RR:
        case DI_MOV_RI:
        case DI_LEA:
        case DI_MOVZX:
        case DI_MOVSX: return BNT_CON_MOV;
        case DI_ADD_RR:
        case DI_ADD_RI: return BNT_CON_ADD;
        case DI_SUB_RR:
        case DI_SUB_RI: return BNT_CON_SUB;
        case DI_AND_RR: return BNT_CON_BIT_AND;
        case DI_OR_RR:  return BNT_CON_BIT_OR;
        case DI_XOR_RR: return BNT_CON_BIT_XOR;
        case DI_SHL_RI: return BNT_CON_SHL;
        case DI_SHR_RI: return BNT_CON_SHR;
        case DI_CMP_RR:
        case DI_CMP_RI:
        case DI_TEST_RR: return BNT_CON_CMP_EQ;
        case DI_IMUL:   return BNT_CON_MUL;
        case DI_IDIV:   return BNT_CON_DIV;
        case DI_PUSH_R: return BNT_CON_PUSH;
        case DI_POP_R:  return BNT_CON_POP;
        case DI_JMP:    return BNT_CON_JMP;
        case DI_JZ:     return BNT_CON_JZ;
        case DI_JNZ:    return BNT_CON_JNZ;
        case DI_CALL:   return BNT_CON_CALL;
        case DI_RET:    return BNT_CON_RET;
        case DI_NOP:    return BNT_CON_NOP;
        case DI_SYSCALL:return BNT_CON_SYSCALL;
        case DI_CDQ:    return BNT_CON_NOP;
        default:        return BNT_CON_NOP;
    }
}

int dfg_to_net(const dfg_program_t *p, bine_net_t *out) {
    if (!p || !out) return -1;
    /* Worst case: each instruction becomes ~4 nodes (def, use1, use2, side) */
    uint32_t cap = (uint32_t)(p->n * 4 + 16);
    if (bine_net_reserve(out, cap, cap * 2, 16) < 0) return -2;

    /* Build a def->use map: for each register, the node that produces
     * its current value. */
    uint32_t reg_value[16];
    for (int i = 0; i < 16; i++) reg_value[i] = 0xFFFFFFFEu;
    uint32_t flag_value = 0xFFFFFFFEu;

    uint32_t n_nodes = 0, n_wires = 0, n_io = 0;
    /* Add a single NOP at the start so node 0 is reserved as "halt sink" */
    out->nodes[n_nodes].type = BNT_CON_NOP;
    out->nodes[n_nodes].flags = 0;
    out->nodes[n_nodes].aux_data = 0;
    out->nodes[n_nodes].principal_wire = 0xFFFFFFFEu;
    uint32_t halt_node = n_nodes++;

    for (size_t i = 0; i < p->n; i++) {
        const dfg_instr_t *ins = &p->items[i];
        uint8_t nt = kind_to_bnt(ins->kind);
        /* Skip NOPs entirely (they have no effect) */
        if (ins->kind == DI_NOP) continue;
        /* Create the primary node for this instruction */
        uint32_t primary = n_nodes++;
        out->nodes[primary].type = nt;
        out->nodes[primary].flags = 0;
        out->nodes[primary].aux_data = (uint16_t)ins->imm;
        out->nodes[primary].principal_wire = 0xFFFFFFFEu;
        /* Add wires from source registers to this instruction's aux ports.
         * Each wire sets the principal_wire of BOTH the producer and
         * consumer nodes so that active pair detection in
         * kolmogorov_step works (which requires principal-to-principal). */
        if (ins->src_reg != 0xFF && reg_value[ins->src_reg] != 0xFFFFFFFEu) {
            uint32_t producer = reg_value[ins->src_reg];
            out->wires[n_wires].node_a = producer;
            out->wires[n_wires].port_a = BWP_PRINCIPAL;
            out->wires[n_wires].node_b = primary;
            out->nodes[producer].principal_wire = n_wires;
            out->nodes[primary].principal_wire = n_wires;
            n_wires++;
        }
        if (ins->src2_reg != 0xFF && reg_value[ins->src2_reg] != 0xFFFFFFFEu) {
            uint32_t producer = reg_value[ins->src2_reg];
            out->wires[n_wires].node_a = producer;
            out->wires[n_wires].port_a = BWP_PRINCIPAL;
            out->wires[n_wires].node_b = primary;
            out->nodes[producer].principal_wire = n_wires;
            out->nodes[primary].principal_wire = n_wires;
            n_wires++;
        }
        /* If this instruction reads its own dst register (e.g. ADD RAX, RBX
         * reads RAX before writing the new value), wire the previous
         * definition as an aux port. */
        if (ins->read_dst && ins->dst_reg != 0xFF &&
            reg_value[ins->dst_reg] != 0xFFFFFFFEu) {
            uint32_t producer = reg_value[ins->dst_reg];
            out->wires[n_wires].node_a = producer;
            out->wires[n_wires].port_a = BWP_PRINCIPAL;
            out->wires[n_wires].node_b = primary;
            out->nodes[producer].principal_wire = n_wires;
            out->nodes[primary].principal_wire = n_wires;
            n_wires++;
        }
        /* Update def map: dst register now points to this primary */
        if (ins->dst_reg != 0xFF) {
            reg_value[ins->dst_reg] = primary;
        }
        /* CMP/TEST produces a flag */
        if (ins->kind == DI_CMP_RR || ins->kind == DI_CMP_RI ||
            ins->kind == DI_TEST_RR) {
            flag_value = primary;
        }
        /* JZ/JNZ consume the flag */
        if (ins->kind == DI_JZ || ins->kind == DI_JNZ) {
            if (flag_value != 0xFFFFFFFEu) {
                out->wires[n_wires].node_a = flag_value;
                out->wires[n_wires].port_a = BWP_PRINCIPAL;
                out->wires[n_wires].node_b = primary;
                out->nodes[flag_value].principal_wire = n_wires;
                n_wires++;
            }
        }
    }
    (void)halt_node; (void)n_io;
    out->num_nodes = n_nodes;
    out->num_wires = n_wires;
    out->num_inputs = 0;
    out->num_outputs = 0;
    return 0;
}

int dfg_build(const uint8_t *text, size_t len, uint64_t base_vaddr,
              bine_net_t *out) {
    dfg_program_t p;
    if (dfg_decode(text, len, base_vaddr, &p) < 0) return -1;
    int rc = dfg_to_net(&p, out);
    dfg_program_free(&p);
    return rc;
}
