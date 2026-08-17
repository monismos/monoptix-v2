/*
 * test_layout.c - Tests for the offset-preserving layout engine
 * (serialize/layout.c): dead-code NOP replacement and safe same-length
 * reordering driven by the STRAND-P schedule order.
 */

#include "bine.h"
#include "icor/bits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(name) static int test_##name(void)
#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

/* xor eax,eax (2) / xor eax,eax (2) / ret (1) — dead middle instruction */
static const uint8_t text_a[] = { 0x31, 0xC0, 0x31, 0xC0, 0xC3 };

static void fill_intrinsic(BinaryIntrinsic *bin, const uint8_t *text, size_t sz,
                           const char *path) {
    memset(bin, 0, sizeof(*bin));
    bin->e_machine = EM_X86_64;
    bin->e_entry = 0;
    bin->text_bytes = (uint8_t *)malloc(sz);
    memcpy(bin->text_bytes, text, sz);
    bin->text_size = sz;
    bin->text_vaddr = 0;
    bin->load_vaddr = 0;
    bin->load_offset = 0;
    bin->path = strdup(path);
    bin->relocs = NULL;
    bin->n_relocs = 0;
    bin->symbols = NULL;
    bin->n_symbols = 0;
}

TEST(dead_agent_becomes_nop) {
    BinaryIntrinsic bin;
    fill_intrinsic(&bin, text_a, sizeof(text_a), "synthetic.bin");

    /* 3 agents (one per decoded insn: xor, xor, ret), agent 1 = ERA */
    uint64_t net[3];
    net[0] = SET_TYPE(0, AGENT_CON);
    net[1] = SET_TYPE(0, AGENT_ERA);
    net[2] = SET_ESSENTIAL(SET_TYPE(0, AGENT_CON)); /* ret */

    uint32_t n_insns = 0;
    Instruction *insns = decode_instructions(bin.text_bytes, bin.text_size, &n_insns);
    ASSERT(insns && n_insns == 3);

    uint8_t *dead = bine_compute_dead_agents(net, 3, insns, n_insns, &bin);
    ASSERT(dead);
    ASSERT(dead[1] == 1);   /* middle xor is dead */
    ASSERT(dead[0] == 0);
    ASSERT(dead[2] == 0);   /* essential ret stays */

    uint8_t out[16];
    uint64_t out_size = 0;
    uint64_t *map = NULL;
    int rc = bine_apply_layout(net, 3, NULL, 0, &bin, out, &out_size, &map);
    ASSERT(rc == 3);
    ASSERT(out_size == bin.text_size);      /* offset-preserving */
    const uint8_t expect[] = { 0x31, 0xC0, 0x66, 0x90, 0xC3 };
    ASSERT(memcmp(out, expect, sizeof(expect)) == 0);
    ASSERT(map[0] == 0 && map[1] == 2 && map[2] == 4);

    free(dead);
    free(map);
    free_instructions(insns);
    free(bin.text_bytes);
    free(bin.path);
    return 0;
}

TEST(no_schedule_no_reorder) {
    BinaryIntrinsic bin;
    fill_intrinsic(&bin, text_a, sizeof(text_a), "synthetic.bin");
    uint64_t net[3];
    for (int i = 0; i < 3; i++) net[i] = SET_TYPE(0, AGENT_CON);
    net[2] = SET_ESSENTIAL(net[2]);

    uint8_t out[16];
    uint64_t out_size = 0;
    uint64_t *map = NULL;
    ASSERT(bine_apply_layout(net, 3, NULL, 0, &bin, out, &out_size, &map) == 3);
    ASSERT(memcmp(out, text_a, sizeof(text_a)) == 0); /* unchanged */
    free(map);
    free(bin.text_bytes);
    free(bin.path);
    return 0;
}

/* mov edx,ecx (89 D1) / mov ebx,eax (89 C3) / ret (C3) — swap first two */
static const uint8_t text_b[] = { 0x89, 0xD1, 0x89, 0xC3, 0xC3 };

TEST(schedule_driven_swap) {
    BinaryIntrinsic bin;
    fill_intrinsic(&bin, text_b, sizeof(text_b), "synthetic.bin");
    uint64_t net[3];
    net[0] = SET_TYPE(0, AGENT_CON); /* mov edx,ecx */
    net[1] = SET_TYPE(0, AGENT_CON); /* mov ebx,eax */
    net[2] = SET_ESSENTIAL(SET_TYPE(0, AGENT_CON)); /* ret */

    /* Schedule order: instruction 1 before instruction 0 */
    uint32_t order[3] = { 1, 0, 2 };

    uint8_t out[16];
    uint64_t out_size = 0;
    uint64_t *map = NULL;
    ASSERT(bine_apply_layout(net, 3, order, 3, &bin, out, &out_size, &map) == 3);
    ASSERT(out_size == bin.text_size);
    const uint8_t expect[] = { 0x89, 0xC3, 0x89, 0xD1, 0xC3 };
    ASSERT(memcmp(out, expect, sizeof(expect)) == 0);
    free(map);
    free(bin.text_bytes);
    free(bin.path);
    return 0;
}

TEST(reg_dependency_blocks_swap) {
    /* mov ebx,ecx (89 D9: def=ebx use=ecx) / mov eax,ebx (89 D8: def=eax use=ebx)
     * — RAW dependency via ebx, must NOT swap. */
    const uint8_t text_c[] = { 0x89, 0xD9, 0x89, 0xD8, 0xC3 };
    BinaryIntrinsic bin;
    fill_intrinsic(&bin, text_c, sizeof(text_c), "synthetic.bin");
    uint64_t net[3];
    net[0] = SET_TYPE(0, AGENT_CON);
    net[1] = SET_TYPE(0, AGENT_CON);
    net[2] = SET_ESSENTIAL(SET_TYPE(0, AGENT_CON));

    uint32_t order[3] = { 1, 0, 2 };
    uint8_t out[16];
    uint64_t out_size = 0;
    uint64_t *map = NULL;
    ASSERT(bine_apply_layout(net, 3, order, 3, &bin, out, &out_size, &map) == 3);
    ASSERT(memcmp(out, text_c, sizeof(text_c)) == 0); /* unchanged */
    free(map);
    free(bin.text_bytes);
    free(bin.path);
    return 0;
}

TEST(branch_target_never_eliminated) {
    /* jmp +1 (EB 00) / xor eax,eax (31 C0) / ret (C3): target = xor.
     * Even if the net says ERA, the target guard keeps it. */
    const uint8_t text_d[] = { 0xEB, 0x00, 0x31, 0xC0, 0xC3 };
    BinaryIntrinsic bin;
    fill_intrinsic(&bin, text_d, sizeof(text_d), "synthetic.bin");
    uint64_t net[3];
    net[0] = SET_ESSENTIAL(SET_TYPE(0, AGENT_CON));
    net[1] = SET_TYPE(0, AGENT_ERA); /* net says dead */
    net[2] = SET_ESSENTIAL(SET_TYPE(0, AGENT_CON));

    uint8_t out[16];
    uint64_t out_size = 0;
    uint64_t *map = NULL;
    ASSERT(bine_apply_layout(net, 3, NULL, 0, &bin, out, &out_size, &map) == 3);
    ASSERT(memcmp(out, text_d, sizeof(text_d)) == 0); /* untouched */
    free(map);
    free(bin.text_bytes);
    free(bin.path);
    return 0;
}

int test_layout_main(void) {
    int failed = 0;
    failed += test_dead_agent_becomes_nop();
    failed += test_no_schedule_no_reorder();
    failed += test_schedule_driven_swap();
    failed += test_reg_dependency_blocks_swap();
    failed += test_branch_target_never_eliminated();
    if (failed) {
        fprintf(stderr, "test_layout: %d test(s) FAILED\n", failed);
        return 1;
    }
    printf("test_layout: all tests PASSED\n");
    return 0;
}

#ifndef BINE_COMBINED_TESTS
int main(void) { return test_layout_main(); }
#endif
