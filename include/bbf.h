/*
 * bbf.h - BINE Binary Format v1
 *
 * Self-describing binary container for interaction nets.
 * The bytes ARE the program: no text, no assembly, no IR.
 *
 *   .bbf file = header + node table + wire table + io table + crc
 *
 * Every .bbf is both source code and machine code. The parser
 * is a struct deserializer; the serializer is identity.
 */

#ifndef BINE_BBF_H
#define BINE_BBF_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BBF_MAGIC   0x31464242u   /* "BBF1" little-endian */
#define BBF_VERSION 0x00010000u   /* 1.0 */
#define BBF_NODE_SZ 8
#define BBF_WIRE_SZ 8
#define BBF_IO_SZ   8

/* Node types (1 byte) */
#define BNT_ERA          0x00   /* eraser: removes 1 wire */
#define BNT_DUP          0x01   /* duplicator: forks 1 wire into 2 */
#define BNT_CON_NAND     0x02   /* boolean NAND */
#define BNT_CON_AND      0x03
#define BNT_CON_OR       0x04
#define BNT_CON_XOR      0x05
#define BNT_CON_NOT      0x06
#define BNT_CON_ADD      0x07
#define BNT_CON_SUB      0x08
#define BNT_CON_MUL      0x09
#define BNT_CON_DIV      0x0A
#define BNT_CON_MOD      0x0B
#define BNT_CON_SHL      0x0C
#define BNT_CON_SHR      0x0D
#define BNT_CON_CMP_EQ   0x0E
#define BNT_CON_CMP_LT   0x0F
#define BNT_CON_CMP_GT   0x10
#define BNT_CON_MOV      0x11
#define BNT_CON_LOAD     0x12
#define BNT_CON_STORE    0x13
#define BNT_CON_JMP      0x14
#define BNT_CON_JZ       0x15
#define BNT_CON_JNZ      0x16
#define BNT_CON_CALL     0x17
#define BNT_CON_RET      0x18
#define BNT_CON_PUSH     0x19
#define BNT_CON_POP      0x1A
#define BNT_CON_SYSCALL  0x1B
#define BNT_CON_NOP      0x1C
#define BNT_CON_HALT     0x1D
#define BNT_CON_MOV_IMM  0x1E
#define BNT_CON_BIT_AND  0x1F
#define BNT_CON_BIT_OR   0x20
#define BNT_CON_BIT_XOR  0x21
#define BNT_CON_BIT_NOT  0x22
#define BNT_USER         0x80   /* user-defined; opcode in aux_data */

/* Node flags (1 byte) */
#define BNF_IS_INPUT   0x01
#define BNF_IS_OUTPUT  0x02
#define BNF_IS_EXTERNAL 0x04
#define BNF_IS_REDUCED 0x08

/* Wire ports (which port of the node this wire touches) */
#define BWP_PRINCIPAL  0
#define BWP_AUX_LEFT   1
#define BWP_AUX_RIGHT  2

/* I/O direction */
#define BIO_IN  0
#define BIO_OUT 1

typedef struct {
    uint8_t  type;            /* BNT_* */
    uint8_t  flags;           /* BNF_* */
    uint16_t aux_data;        /* opcode-specific (e.g. imm16, syscall nr) */
    uint32_t principal_wire;  /* index into wire table; 0xFFFFFFFF = NIL */
} bbf_node_t;

typedef struct {
    uint32_t node_a;          /* node index */
    uint16_t port_a;          /* BWP_* */
    uint16_t node_b;          /* node index */
} bbf_wire_t;

typedef struct {
    uint32_t node;            /* node index */
    uint8_t  port;            /* BWP_* */
    uint8_t  direction;       /* BIO_IN / BIO_OUT */
    uint16_t reserved;
} bbf_io_t;

/* In-memory net */
typedef struct {
    uint32_t     num_nodes;
    uint32_t     num_wires;
    uint32_t     num_inputs;
    uint32_t     num_outputs;
    bbf_node_t  *nodes;
    bbf_wire_t  *wires;
    bbf_io_t    *ios;
    uint32_t     crc;
} bine_net_t;

/* API */
int  bine_net_init(bine_net_t *net);
void bine_net_free(bine_net_t *net);
int  bine_net_reserve(bine_net_t *net, uint32_t nodes, uint32_t wires, uint32_t ios);

/* Parse bytes into net. Returns 0 on success. */
int  bine_parse_bbf(const uint8_t *src, size_t len, bine_net_t *out);

/* Serialize net into bytes. Caller frees *out. */
int  bine_serialize_bbf(const bine_net_t *net, uint8_t **out, size_t *out_len);

/* Validate net (cycle detection, port consistency). */
int  bine_net_validate(const bine_net_t *net);

/* Print human-readable summary (for debug only). */
void bine_net_dump(const bine_net_t *net, FILE *f);

/* CRC32 (IEEE) */
uint32_t bine_crc32(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* BINE_BBF_H */
