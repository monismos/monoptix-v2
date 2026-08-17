/*
 * test_dfg.c - Tests for DFG construction from x86-64
 */

#include "dfg.h"
#include "bbf.h"
#include <stdio.h>
#include <string.h>

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

/* Hand-crafted x86-64 bytes for "MOV RAX, RBX; RET" */
static const uint8_t mov_rax_rbx_ret[] = {
    0x48, 0x89, 0xD8,  /* MOV RAX, RBX (REX.W=1, opcode=89, ModRM=11 011 000) */
    0xC3              /* RET */
};

int test_decode_mov_ret(void) {
    dfg_program_t p;
    int rc = dfg_decode(mov_rax_rbx_ret, sizeof(mov_rax_rbx_ret), 0, &p);
    ASSERT(rc == 0);
    ASSERT(p.n == 2);
    ASSERT(p.items[0].kind == DI_MOV_RR);
    ASSERT(p.items[0].dst_reg == 0);  /* RAX */
    ASSERT(p.items[0].src_reg == 3);  /* RBX */
    ASSERT(p.items[1].kind == DI_RET);
    dfg_program_free(&p);
    return 0;
}

int test_decode_nop(void) {
    const uint8_t nop[] = { 0x90 };
    dfg_program_t p;
    ASSERT(dfg_decode(nop, 1, 0, &p) == 0);
    ASSERT(p.n == 1);
    ASSERT(p.items[0].kind == DI_NOP);
    dfg_program_free(&p);
    return 0;
}

int test_decode_ret(void) {
    const uint8_t ret[] = { 0xC3 };
    dfg_program_t p;
    ASSERT(dfg_decode(ret, 1, 0, &p) == 0);
    ASSERT(p.n == 1);
    ASSERT(p.items[0].kind == DI_RET);
    dfg_program_free(&p);
    return 0;
}

int test_dfg_to_net_mov_ret(void) {
    dfg_program_t p;
    dfg_decode(mov_rax_rbx_ret, sizeof(mov_rax_rbx_ret), 0, &p);
    bine_net_t net;
    int rc = dfg_to_net(&p, &net);
    ASSERT(rc == 0);
    /* Should have at least: 1 MOV node + 1 RET node */
    ASSERT(net.num_nodes >= 2);
    /* For "mov rax, rbx; ret" there are no reads of RAX, so no
     * def-use wires.  Verify the structure anyway. */
    bine_net_free(&net);
    dfg_program_free(&p);
    return 0;
}

int test_dfg_build_full(void) {
    bine_net_t net;
    int rc = dfg_build(mov_rax_rbx_ret, sizeof(mov_rax_rbx_ret), 0, &net);
    ASSERT(rc == 0);
    ASSERT(net.num_nodes >= 1);
    bine_net_free(&net);
    return 0;
}

int test_decode_complex_program(void) {
    /* MOV RAX, 42; ADD RAX, RBX; RET */
    const uint8_t prog[] = {
        0x48, 0xC7, 0xC0, 0x2A, 0x00, 0x00, 0x00,  /* MOV RAX, 42 */
        0x48, 0x01, 0xD8,                           /* ADD RAX, RBX */
        0xC3                                         /* RET */
    };
    dfg_program_t p;
    int rc = dfg_decode(prog, sizeof(prog), 0, &p);
    ASSERT(rc == 0);
    ASSERT(p.n >= 3);
    bine_net_t net;
    ASSERT(dfg_to_net(&p, &net) == 0);
    /* Should have a wire connecting MOV -> ADD (def-use chain) */
    ASSERT(net.num_wires >= 1);
    bine_net_free(&net);
    dfg_program_free(&p);
    return 0;
}

int test_dfg_main(void) {
    int failed = 0;
    failed += test_decode_mov_ret();
    failed += test_decode_nop();
    failed += test_decode_ret();
    failed += test_dfg_to_net_mov_ret();
    failed += test_dfg_build_full();
    failed += test_decode_complex_program();
    if (failed) {
        fprintf(stderr, "test_dfg: %d test(s) FAILED\n", failed);
        return 1;
    }
    printf("test_dfg: all tests PASSED\n");
    return 0;
}

#ifndef BINE_COMBINED_TESTS
int main(void) { return test_dfg_main(); }
#endif
