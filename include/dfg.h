/*
 * dfg.h - Data-Flow Graph from x86-64 instructions
 *
 * This replaces the broken binary_to_icor.c with a real, wire-connected
 * ICOR construction.  Each instruction becomes one or more nodes with
 * explicit wire connections via def-use chains.
 *
 * The output is a bine_net_t that is semantically equivalent to the
 * input instruction stream.
 */

#ifndef BINE_DFG_H
#define BINE_DFG_H

#include "bbf.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decoded x86-64 instruction (minimal subset for the 30 most common ops). */
typedef enum {
    DI_MOV_RR,    /* MOV r64, r64 */
    DI_MOV_RI,    /* MOV r64, imm32 */
    DI_ADD_RR,    /* ADD r64, r64 */
    DI_ADD_RI,    /* ADD r64, imm32 */
    DI_SUB_RR,
    DI_SUB_RI,
    DI_AND_RR,
    DI_OR_RR,
    DI_XOR_RR,
    DI_SHL_RI,
    DI_SHR_RI,
    DI_CMP_RR,
    DI_CMP_RI,
    DI_TEST_RR,
    DI_PUSH_R,
    DI_POP_R,
    DI_JMP,       /* unconditional */
    DI_JZ,        /* je */
    DI_JNZ,       /* jne */
    DI_CALL,
    DI_RET,
    DI_NOP,
    DI_SYSCALL,
    DI_LEA,
    DI_MOVZX,
    DI_MOVSX,
    DI_IMUL,
    DI_IDIV,
    DI_CDQ,
    DI_UNKNOWN
} dfg_instr_kind_t;

typedef struct {
    dfg_instr_kind_t kind;
    uint8_t  dst_reg;     /* 0..15 (rax..r15) or 0xFF = none */
    uint8_t  src_reg;     /* 0..15 or 0xFF = none */
    uint8_t  src2_reg;    /* 0..15 or 0xFF = none */
    uint8_t  read_dst;    /* 1 if dst_reg is also a source (e.g. ADD) */
    int32_t  imm;         /* immediate, or 0 */
    uint64_t address;     /* virtual address (for relocations) */
} dfg_instr_t;

/* Decoded x86-64 stream (slim wrapper). */
typedef struct {
    dfg_instr_t *items;
    size_t       n;
} dfg_program_t;

int dfg_program_init(dfg_program_t *p);
void dfg_program_free(dfg_program_t *p);

/* Decode raw .text bytes into a dfg_program.  Length decoder is from
 * src/binary/length.c.  This only handles the 30 instructions above; for
 * anything else it emits DI_UNKNOWN. */
int dfg_decode(const uint8_t *text, size_t len, uint64_t base_vaddr,
               dfg_program_t *out);

/* Build a wire-connected ICOR net from a decoded program. */
int dfg_to_net(const dfg_program_t *p, bine_net_t *out);

/* Build a complete optimized pipeline:
 *   .text bytes -> DFG program -> wire-connected net
 */
int dfg_build(const uint8_t *text, size_t len, uint64_t base_vaddr,
              bine_net_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BINE_DFG_H */
