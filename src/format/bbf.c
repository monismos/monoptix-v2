/*
 * bbf.c - BINE Binary Format v1 implementation
 */

#include "bbf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---------- CRC32 (IEEE) ---------- */
static uint32_t crc32_table[256];
static int      crc32_init_done = 0;

static void crc32_init(void) {
    if (crc32_init_done) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_init_done = 1;
}

uint32_t bine_crc32(const uint8_t *data, size_t len) {
    crc32_init();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---------- Net lifecycle ---------- */
int bine_net_init(bine_net_t *net) {
    memset(net, 0, sizeof(*net));
    return 0;
}

void bine_net_free(bine_net_t *net) {
    if (!net) return;
    free(net->nodes); net->nodes = NULL;
    free(net->wires); net->wires = NULL;
    free(net->ios);   net->ios   = NULL;
    net->num_nodes = net->num_wires = net->num_inputs = net->num_outputs = 0;
}

int bine_net_reserve(bine_net_t *net, uint32_t nodes, uint32_t wires, uint32_t ios) {
    net->nodes = (bbf_node_t*)calloc(nodes ? nodes : 1, sizeof(bbf_node_t));
    net->wires = (bbf_wire_t*)calloc(wires ? wires : 1, sizeof(bbf_wire_t));
    net->ios   = (bbf_io_t*)calloc(ios   ? ios   : 1, sizeof(bbf_io_t));
    if (!net->nodes || !net->wires || !net->ios) {
        bine_net_free(net);
        return -1;
    }
    net->num_nodes   = nodes;
    net->num_wires   = wires;
    net->num_inputs  = 0;
    net->num_outputs = 0;
    for (uint32_t i = 0; i < nodes; i++)
        net->nodes[i].principal_wire = 0xFFFFFFFFu;
    return 0;
}

/* ---------- Parser ---------- */
int bine_parse_bbf(const uint8_t *src, size_t len, bine_net_t *out) {
    if (!src || !out) return -1;
    bine_net_init(out);

    /* Minimum size: 16 (header) + 16 (counts) */
    if (len < 32) return -2;

    uint32_t magic    = (uint32_t)src[0] | ((uint32_t)src[1] << 8)
                      | ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
    uint32_t version  = (uint32_t)src[4] | ((uint32_t)src[5] << 8)
                      | ((uint32_t)src[6] << 16) | ((uint32_t)src[7] << 24);
    /* flags @ offset 8, reserved @ offset 12 (ignored) */
    if (magic != BBF_MAGIC)   return -3;
    if (version != BBF_VERSION) return -4;

    uint32_t nn = (uint32_t)src[16] | ((uint32_t)src[17] << 8)
                | ((uint32_t)src[18] << 16) | ((uint32_t)src[19] << 24);
    uint32_t nw = (uint32_t)src[20] | ((uint32_t)src[21] << 8)
                | ((uint32_t)src[22] << 16) | ((uint32_t)src[23] << 24);
    uint32_t ni = (uint32_t)src[24] | ((uint32_t)src[25] << 8)
                | ((uint32_t)src[26] << 16) | ((uint32_t)src[27] << 24);
    uint32_t no = (uint32_t)src[28] | ((uint32_t)src[29] << 8)
                | ((uint32_t)src[30] << 16) | ((uint32_t)src[31] << 24);

    size_t expected = 32
                    + (size_t)nn * BBF_NODE_SZ
                    + (size_t)nw * BBF_WIRE_SZ
                    + (size_t)(ni + no) * BBF_IO_SZ
                    + 4;
    if (len < expected) return -5;

    if (bine_net_reserve(out, nn, nw, ni + no) < 0) return -6;
    out->num_inputs  = ni;
    out->num_outputs = no;

    const uint8_t *p = src + 32;
    for (uint32_t i = 0; i < nn; i++) {
        out->nodes[i].type           = p[0];
        out->nodes[i].flags          = p[1];
        out->nodes[i].aux_data       = (uint16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
        out->nodes[i].principal_wire = (uint32_t)p[4] | ((uint32_t)p[5] << 8)
                                     | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        p += BBF_NODE_SZ;
    }
    for (uint32_t i = 0; i < nw; i++) {
        out->wires[i].node_a = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                             | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        out->wires[i].port_a = (uint16_t)((uint16_t)p[4] | ((uint16_t)p[5] << 8));
        out->wires[i].node_b = (uint32_t)p[6] | ((uint32_t)p[7] << 8); /* upper 2 bytes zero */
        p += BBF_WIRE_SZ;
    }
    for (uint32_t i = 0; i < ni + no; i++) {
        out->ios[i].node      = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                              | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        out->ios[i].port      = p[4];
        out->ios[i].direction = p[5];
        out->ios[i].reserved  = (uint16_t)((uint16_t)p[6] | ((uint16_t)p[7] << 8));
        p += BBF_IO_SZ;
    }

    /* CRC covers everything except the trailing 4 CRC bytes */
    out->crc = bine_crc32(src, expected - 4);
    uint32_t stored_crc = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                        | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    if (stored_crc != out->crc) {
        bine_net_free(out);
        return -7; /* CRC mismatch */
    }

    return 0;
}

/* ---------- Serializer ---------- */
static void w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int bine_serialize_bbf(const bine_net_t *net, uint8_t **out, size_t *out_len) {
    if (!net || !out || !out_len) return -1;
    size_t len = 32
               + (size_t)net->num_nodes * BBF_NODE_SZ
               + (size_t)net->num_wires * BBF_WIRE_SZ
               + (size_t)(net->num_inputs + net->num_outputs) * BBF_IO_SZ
               + 4;
    uint8_t *buf = (uint8_t*)malloc(len);
    if (!buf) return -2;
    memset(buf, 0, len);

    w32(buf + 0,  BBF_MAGIC);
    w32(buf + 4,  BBF_VERSION);
    w32(buf + 8,  0);   /* flags */
    w32(buf + 12, 0);   /* reserved */
    w32(buf + 16, net->num_nodes);
    w32(buf + 20, net->num_wires);
    w32(buf + 24, net->num_inputs);
    w32(buf + 28, net->num_outputs);

    uint8_t *p = buf + 32;
    for (uint32_t i = 0; i < net->num_nodes; i++) {
        p[0] = net->nodes[i].type;
        p[1] = net->nodes[i].flags;
        p[2] = (uint8_t)(net->nodes[i].aux_data);
        p[3] = (uint8_t)(net->nodes[i].aux_data >> 8);
        w32(p + 4, net->nodes[i].principal_wire);
        p += BBF_NODE_SZ;
    }
    for (uint32_t i = 0; i < net->num_wires; i++) {
        w32(p,      net->wires[i].node_a);
        p[4] = (uint8_t)(net->wires[i].port_a);
        p[5] = (uint8_t)(net->wires[i].port_a >> 8);
        w32(p + 6,  net->wires[i].node_b);
        p += BBF_WIRE_SZ;
    }
    for (uint32_t i = 0; i < net->num_inputs + net->num_outputs; i++) {
        w32(p,      net->ios[i].node);
        p[4] = net->ios[i].port;
        p[5] = net->ios[i].direction;
        p[6] = (uint8_t)(net->ios[i].reserved);
        p[7] = (uint8_t)(net->ios[i].reserved >> 8);
        p += BBF_IO_SZ;
    }
    uint32_t crc = bine_crc32(buf, len - 4);
    w32(p, crc);
    *out = buf;
    *out_len = len;
    return 0;
}

/* ---------- Validation ---------- */
int bine_net_validate(const bine_net_t *net) {
    if (!net) return -1;
    /* Check node indices in wires */
    for (uint32_t i = 0; i < net->num_wires; i++) {
        if (net->wires[i].node_a >= net->num_nodes) return -10;
        if (net->wires[i].node_b >= net->num_nodes) return -11;
        if (net->wires[i].port_a > 2) return -12;
    }
    /* Check principal_wire references */
    for (uint32_t i = 0; i < net->num_nodes; i++) {
        uint32_t pw = net->nodes[i].principal_wire;
        if (pw != 0xFFFFFFFFu && pw >= net->num_wires) return -13;
    }
    /* Check IO references */
    for (uint32_t i = 0; i < net->num_inputs + net->num_outputs; i++) {
        if (net->ios[i].node >= net->num_nodes) return -14;
        if (net->ios[i].port > 2) return -15;
    }
    return 0;
}

/* ---------- Debug dump ---------- */
static const char *node_type_name(uint8_t t) {
    switch (t) {
        case BNT_ERA:         return "ERA";
        case BNT_DUP:         return "DUP";
        case BNT_CON_NAND:    return "NAND";
        case BNT_CON_AND:     return "AND";
        case BNT_CON_OR:      return "OR";
        case BNT_CON_XOR:     return "XOR";
        case BNT_CON_NOT:     return "NOT";
        case BNT_CON_ADD:     return "ADD";
        case BNT_CON_SUB:     return "SUB";
        case BNT_CON_MUL:     return "MUL";
        case BNT_CON_DIV:     return "DIV";
        case BNT_CON_MOD:     return "MOD";
        case BNT_CON_SHL:     return "SHL";
        case BNT_CON_SHR:     return "SHR";
        case BNT_CON_CMP_EQ:  return "CMP_EQ";
        case BNT_CON_CMP_LT:  return "CMP_LT";
        case BNT_CON_CMP_GT:  return "CMP_GT";
        case BNT_CON_MOV:     return "MOV";
        case BNT_CON_LOAD:    return "LOAD";
        case BNT_CON_STORE:   return "STORE";
        case BNT_CON_JMP:     return "JMP";
        case BNT_CON_JZ:      return "JZ";
        case BNT_CON_JNZ:     return "JNZ";
        case BNT_CON_CALL:    return "CALL";
        case BNT_CON_RET:     return "RET";
        case BNT_CON_PUSH:    return "PUSH";
        case BNT_CON_POP:     return "POP";
        case BNT_CON_SYSCALL: return "SYSCALL";
        case BNT_CON_NOP:     return "NOP";
        case BNT_CON_HALT:    return "HALT";
        case BNT_CON_MOV_IMM: return "MOV_IMM";
        case BNT_CON_BIT_AND: return "BIT_AND";
        case BNT_CON_BIT_OR:  return "BIT_OR";
        case BNT_CON_BIT_XOR: return "BIT_XOR";
        case BNT_CON_BIT_NOT: return "BIT_NOT";
        default:              return "???";
    }
}

void bine_net_dump(const bine_net_t *net, FILE *f) {
    if (!net || !f) return;
    fprintf(f, "=== BBF net ===\n");
    fprintf(f, "  nodes:   %u\n", net->num_nodes);
    fprintf(f, "  wires:   %u\n", net->num_wires);
    fprintf(f, "  inputs:  %u\n", net->num_inputs);
    fprintf(f, "  outputs: %u\n", net->num_outputs);
    fprintf(f, "  crc:     0x%08X\n", net->crc);
    fprintf(f, "  nodes:\n");
    for (uint32_t i = 0; i < net->num_nodes; i++) {
        fprintf(f, "    [%u] %-10s flags=0x%02X aux=0x%04X pw=%s\n",
                i, node_type_name(net->nodes[i].type),
                net->nodes[i].flags, net->nodes[i].aux_data,
                net->nodes[i].principal_wire == 0xFFFFFFFFu ? "NIL" : "");
    }
    fprintf(f, "  wires:\n");
    for (uint32_t i = 0; i < net->num_wires; i++) {
        fprintf(f, "    [%u] n%u.p%u <-> n%u\n",
                i, net->wires[i].node_a, net->wires[i].port_a,
                net->wires[i].node_b);
    }
    if (net->num_inputs + net->num_outputs > 0) {
        fprintf(f, "  io:\n");
        for (uint32_t i = 0; i < net->num_inputs; i++)
            fprintf(f, "    IN  n%u.p%u\n", net->ios[i].node, net->ios[i].port);
        for (uint32_t i = 0; i < net->num_outputs; i++) {
            uint32_t k = net->num_inputs + i;
            fprintf(f, "    OUT n%u.p%u\n", net->ios[k].node, net->ios[k].port);
        }
    }
}
