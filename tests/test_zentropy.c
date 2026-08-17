/*
 * test_zentropy.c - Tests for Z-entropy encoding
 */

#include "zentropy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int test_default_table(void) {
    zentropy_table_t t;
    ASSERT(zentropy_table_default(&t) == 0);
    ASSERT(t.num_symbols == ZENTROPY_MAX_SYMBOLS);
    /* After finalization, every symbol should have a code */
    for (uint16_t k = 0; k < 32; k++) {
        ASSERT(t.len[k] > 0);  /* has a code */
    }
    return 0;
}

int test_observe_finalize(void) {
    zentropy_table_t t;
    zentropy_table_init(&t);
    /* Five symbols with skewed frequencies.  Huffman assigns shorter
     * codes to more frequent symbols.  The exact lengths depend on the
     * algorithm but monotonicity (non-strict) always holds. */
    for (int i = 0; i < 100; i++) zentropy_table_observe(&t, 5);
    for (int i = 0; i < 30;  i++) zentropy_table_observe(&t, 10);
    for (int i = 0; i < 10;  i++) zentropy_table_observe(&t, 15);
    for (int i = 0; i < 3;   i++) zentropy_table_observe(&t, 20);
    for (int i = 0; i < 1;   i++) zentropy_table_observe(&t, 25);
    ASSERT(zentropy_table_finalize(&t) == 0);
    /* Monotonicity: more frequent -> shorter (or equal) */
    ASSERT(t.len[5] <= t.len[10]);
    ASSERT(t.len[10] <= t.len[15]);
    ASSERT(t.len[15] <= t.len[20]);
    ASSERT(t.len[20] <= t.len[25]);
    return 0;
}

int test_encode_decode_round_trip(void) {
    zentropy_table_t t;
    zentropy_table_init(&t);
    for (int i = 0; i < 50; i++) zentropy_table_observe(&t, 7);
    for (int i = 0; i < 20; i++) zentropy_table_observe(&t, 13);
    for (int i = 0; i < 5;  i++) zentropy_table_observe(&t, 21);
    zentropy_table_finalize(&t);
    uint16_t keys[] = { 7, 7, 13, 7, 21, 13, 7, 7, 7, 13 };
    size_t n = sizeof(keys) / sizeof(keys[0]);
    uint8_t *out; size_t out_bits;
    ASSERT(zentropy_encode(&t, keys, n, &out, &out_bits) == 0);
    ASSERT(out_bits > 0);
    uint16_t decoded[10];
    ASSERT(zentropy_decode(&t, out, out_bits, decoded, n) == (int)n);
    for (size_t i = 0; i < n; i++) ASSERT(decoded[i] == keys[i]);
    free(out);
    return 0;
}

int test_self_info(void) {
    zentropy_table_t t;
    zentropy_table_init(&t);
    /* P(5) = 100/101, P(10) = 1/101
     * Self-info(5) = -log2(100/101) ≈ 0.014
     * Self-info(10) = -log2(1/101) ≈ 6.66 */
    for (int i = 0; i < 100; i++) zentropy_table_observe(&t, 5);
    for (int i = 0; i < 1;   i++) zentropy_table_observe(&t, 10);
    uint32_t z5  = zentropy_self_info(&t, 5);
    uint32_t z10 = zentropy_self_info(&t, 10);
    /* The integer approximation: z10 > z5 because 10 is rarer. */
    ASSERT(z10 > z5);
    return 0;
}

int test_cost_bound(void) {
    zentropy_table_t t;
    zentropy_table_init(&t);
    for (int i = 0; i < 50; i++) zentropy_table_observe(&t, 1);
    for (int i = 0; i < 50; i++) zentropy_table_observe(&t, 2);
    zentropy_table_finalize(&t);
    uint16_t keys[100];
    for (int i = 0; i < 100; i++) keys[i] = (i & 1) ? 2 : 1;
    uint64_t cost = zentropy_encode_cost(&t, keys, 100);
    /* Two symbols with equal probability should each cost 1 bit,
     * so total cost should be 100. */
    ASSERT(cost == 100);
    return 0;
}

int test_zentropy_main(void) {
    int failed = 0;
    failed += test_default_table();
    failed += test_observe_finalize();
    failed += test_encode_decode_round_trip();
    failed += test_self_info();
    failed += test_cost_bound();
    if (failed) {
        fprintf(stderr, "test_zentropy: %d test(s) FAILED\n", failed);
        return 1;
    }
    printf("test_zentropy: all tests PASSED\n");
    return 0;
}

#ifndef BINE_COMBINED_TESTS
int main(void) { return test_zentropy_main(); }
#endif
