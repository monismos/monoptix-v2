/*
 * test_bbf.c - Tests for the BINE Binary Format
 */

#include "bbf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TEST(name) static int test_##name(void)
#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

TEST(empty_net_round_trip) {
    bine_net_t net;
    bine_net_init(&net);
    bine_net_reserve(&net, 0, 0, 0);
    uint8_t *out; size_t out_len;
    ASSERT(bine_serialize_bbf(&net, &out, &out_len) == 0);
    ASSERT(out_len == 32 + 4);
    bine_net_t parsed;
    ASSERT(bine_parse_bbf(out, out_len, &parsed) == 0);
    ASSERT(parsed.num_nodes == 0);
    ASSERT(parsed.num_wires == 0);
    free(out);
    bine_net_free(&parsed);
    bine_net_free(&net);
    return 0;
}

TEST(simple_net_round_trip) {
    bine_net_t net;
    bine_net_init(&net);
    bine_net_reserve(&net, 3, 2, 1);
    net.num_nodes = 3;
    net.num_wires = 2;
    net.num_inputs = 0;
    net.num_outputs = 1;
    net.nodes[0].type = BNT_CON_ADD;
    net.nodes[0].flags = BNF_IS_OUTPUT;
    net.nodes[0].aux_data = 0;
    net.nodes[0].principal_wire = 0;
    net.nodes[1].type = BNT_CON_MOV_IMM;
    net.nodes[1].aux_data = 42;
    net.nodes[1].principal_wire = 0;
    net.nodes[2].type = BNT_CON_MOV_IMM;
    net.nodes[2].aux_data = 8;
    net.nodes[2].principal_wire = 1;
    net.wires[0].node_a = 0; net.wires[0].port_a = BWP_PRINCIPAL; net.wires[0].node_b = 1;
    net.wires[1].node_a = 0; net.wires[1].port_a = BWP_AUX_LEFT;  net.wires[1].node_b = 2;
    net.ios[0].node = 0; net.ios[0].port = BWP_PRINCIPAL; net.ios[0].direction = BIO_OUT;
    uint8_t *out; size_t out_len;
    ASSERT(bine_serialize_bbf(&net, &out, &out_len) == 0);
    bine_net_t parsed;
    ASSERT(bine_parse_bbf(out, out_len, &parsed) == 0);
    ASSERT(parsed.num_nodes == 3);
    ASSERT(parsed.num_wires == 2);
    ASSERT(parsed.num_outputs == 1);
    ASSERT(parsed.nodes[0].type == BNT_CON_ADD);
    ASSERT(parsed.nodes[1].aux_data == 42);
    ASSERT(parsed.wires[0].node_a == 0);
    free(out);
    bine_net_free(&parsed);
    bine_net_free(&net);
    return 0;
}

TEST(invalid_magic) {
    uint8_t buf[64] = {0};
    buf[0] = 'X'; buf[1] = 'X'; buf[2] = 'X'; buf[3] = 'X';
    bine_net_t net;
    int rc = bine_parse_bbf(buf, sizeof(buf), &net);
    ASSERT(rc < 0);
    return 0;
}

TEST(crc_mismatch) {
    bine_net_t net;
    bine_net_init(&net);
    bine_net_reserve(&net, 1, 0, 0);
    net.num_nodes = 1;
    net.num_wires = 0;
    net.nodes[0].type = BNT_CON_NOP;
    net.nodes[0].principal_wire = 0xFFFFFFFFu;
    uint8_t *out; size_t out_len;
    bine_serialize_bbf(&net, &out, &out_len);
    /* Corrupt a byte in the middle */
    out[32] ^= 0xFF;
    bine_net_t parsed;
    int rc = bine_parse_bbf(out, out_len, &parsed);
    ASSERT(rc == -7);  /* CRC mismatch */
    free(out);
    bine_net_free(&net);
    return 0;
}

TEST(crc32_known_value) {
    /* CRC32 of "123456789" = 0xCBF43926 */
    const char *s = "123456789";
    uint32_t crc = bine_crc32((const uint8_t*)s, 9);
    ASSERT(crc == 0xCBF43926u);
    return 0;
}

TEST(validate_good_net) {
    bine_net_t net;
    bine_net_init(&net);
    bine_net_reserve(&net, 2, 1, 0);
    net.num_nodes = 2; net.num_wires = 1;
    net.nodes[0].type = BNT_CON_ADD; net.nodes[0].principal_wire = 0;
    net.nodes[1].type = BNT_DUP;     net.nodes[1].principal_wire = 0;
    net.wires[0].node_a = 0; net.wires[0].port_a = BWP_PRINCIPAL;
    net.wires[0].node_b = 1;
    ASSERT(bine_net_validate(&net) == 0);
    bine_net_free(&net);
    return 0;
}

TEST(validate_bad_index) {
    bine_net_t net;
    bine_net_init(&net);
    bine_net_reserve(&net, 1, 1, 0);
    net.num_nodes = 1; net.num_wires = 1;
    net.nodes[0].type = BNT_CON_ADD; net.nodes[0].principal_wire = 0;
    net.wires[0].node_a = 5; /* out of range */
    net.wires[0].node_b = 0;
    ASSERT(bine_net_validate(&net) < 0);
    bine_net_free(&net);
    return 0;
}

int test_bbf_main(void) {
    int failed = 0;
    failed += test_empty_net_round_trip();
    failed += test_simple_net_round_trip();
    failed += test_invalid_magic();
    failed += test_crc_mismatch();
    failed += test_crc32_known_value();
    failed += test_validate_good_net();
    failed += test_validate_bad_index();
    if (failed) {
        fprintf(stderr, "test_bbf: %d test(s) FAILED\n", failed);
        return 1;
    }
    printf("test_bbf: all tests PASSED\n");
    return 0;
}

#ifndef BINE_COMBINED_TESTS
int main(void) { return test_bbf_main(); }
#endif
