/*
 * test_bbf_fuzz.c - Fuzz the .bbf parser: random inputs must never crash,
 * and random valid nets must round-trip losslessly.
 */

#include "bbf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(name) static int test_##name(void)
#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static uint64_t rng_state = 0x123456789ABCDEFULL;
static uint64_t rng_next(void) {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

TEST(random_bytes_no_crash) {
    for (int iter = 0; iter < 2000; iter++) {
        size_t len = (size_t)(rng_next() % 4096);
        uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
        ASSERT(buf != NULL);
        for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)rng_next();
        /* Sometimes start with a valid magic to exercise deep parsing */
        if ((iter % 3) == 0 && len >= 4) {
            buf[0] = 'B'; buf[1] = 'I'; buf[2] = 'N'; buf[3] = 'E';
        }
        bine_net_t net;
        int rc = bine_parse_bbf(buf, len, &net);
        if (rc == 0) bine_net_free(&net); /* valid parse: must be freeable */
        free(buf);
    }
    return 0;
}

TEST(random_nets_roundtrip) {
    for (int iter = 0; iter < 500; iter++) {
        bine_net_t net;
        bine_net_init(&net);
        uint32_t n_nodes = (uint32_t)(rng_next() % 32);
        uint32_t n_wires = (uint32_t)(rng_next() % (n_nodes * 2 + 1));
        uint32_t n_ios   = (uint32_t)(rng_next() % (n_nodes + 1));
        bine_net_reserve(&net, n_nodes, n_wires, n_ios);
        net.num_nodes = n_nodes;
        net.num_wires = n_wires;
        net.num_inputs = 0;
        net.num_outputs = n_ios;
        for (uint32_t i = 0; i < n_nodes; i++) {
            net.nodes[i].type = (uint8_t)(rng_next() % (BNT_CON_NOP + 1));
            net.nodes[i].flags = (uint8_t)(rng_next() & 0x3);
            net.nodes[i].aux_data = (uint16_t)(rng_next() & 0xFFFF);
            net.nodes[i].principal_wire = (uint32_t)(rng_next() % (n_wires + 1));
        }
        for (uint32_t i = 0; i < n_wires; i++) {
            net.wires[i].node_a = (uint32_t)(rng_next() % (n_nodes + 1));
            net.wires[i].port_a = (uint16_t)(rng_next() & 0x3);
            net.wires[i].node_b = (uint32_t)(rng_next() % (n_nodes + 1));
        }
        for (uint32_t i = 0; i < n_ios; i++) {
            net.ios[i].node = (uint32_t)(rng_next() % (n_nodes + 1));
            net.ios[i].port = (uint8_t)(rng_next() & 0x3);
            net.ios[i].direction = (rng_next() & 1) ? BIO_IN : BIO_OUT;
        }

        uint8_t *out = NULL;
        size_t out_len = 0;
        /* Serialization of a net with out-of-range indices must fail
         * gracefully (validation), not crash. */
        bine_net_validate(&net);
        if (bine_serialize_bbf(&net, &out, &out_len) != 0) {
            bine_net_free(&net);
            continue;
        }

        bine_net_t parsed;
        ASSERT(bine_parse_bbf(out, out_len, &parsed) == 0);
        ASSERT(parsed.num_nodes == net.num_nodes);
        ASSERT(parsed.num_wires == net.num_wires);
        ASSERT(parsed.num_outputs == net.num_outputs);
        for (uint32_t i = 0; i < parsed.num_nodes; i++) {
            ASSERT(parsed.nodes[i].type == net.nodes[i].type);
            ASSERT(parsed.nodes[i].aux_data == net.nodes[i].aux_data);
        }
        bine_net_free(&parsed);
        free(out);
        bine_net_free(&net);
    }
    return 0;
}

TEST(corrupt_valid_stream) {
    bine_net_t net;
    bine_net_init(&net);
    bine_net_reserve(&net, 4, 3, 1);
    net.num_nodes = 4;
    net.num_wires = 3;
    net.num_outputs = 1;
    for (uint32_t i = 0; i < net.num_nodes; i++) {
        net.nodes[i].type = BNT_CON_ADD;
        net.nodes[i].principal_wire = i % 3;
    }
    for (uint32_t i = 0; i < net.num_wires; i++) {
        net.wires[i].node_a = i;
        net.wires[i].node_b = i + 1;
    }
    net.ios[0].node = 0;
    net.ios[0].port = BWP_PRINCIPAL;
    net.ios[0].direction = BIO_OUT;

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT(bine_serialize_bbf(&net, &out, &out_len) == 0);

    for (size_t pos = 0; pos < out_len; pos += 7) {
        uint8_t *mut = (uint8_t *)malloc(out_len);
        memcpy(mut, out, out_len);
        mut[pos] ^= (uint8_t)(0x55 + pos);
        bine_net_t parsed;
        int rc = bine_parse_bbf(mut, out_len, &parsed);
        if (rc == 0) bine_net_free(&parsed); /* must not leak/crash */
        free(mut);
    }
    free(out);
    bine_net_free(&net);
    return 0;
}

int test_bbf_fuzz_main(void) {
    int failed = 0;
    failed += test_random_bytes_no_crash();
    failed += test_random_nets_roundtrip();
    failed += test_corrupt_valid_stream();
    if (failed) {
        fprintf(stderr, "test_bbf_fuzz: %d test(s) FAILED\n", failed);
        return 1;
    }
    printf("test_bbf_fuzz: all tests PASSED\n");
    return 0;
}

#ifndef BINE_COMBINED_TESTS
int main(void) { return test_bbf_fuzz_main(); }
#endif
