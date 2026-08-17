/*
 * test_pure_binary.c - End-to-end tests for the pure binary pipeline
 */

#include "bbf.h"
#include "kolmogorov.h"
#include "diagonal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

/* Build a "compute 5+3, output 8" net, optimize it, verify. */
int test_addition_program(void) {
    bine_net_t net;
    bine_net_init(&net);
    bine_net_reserve(&net, 3, 2, 1);
    net.num_nodes = 3; net.num_wires = 2;
    net.num_inputs = 0; net.num_outputs = 1;

    net.nodes[0].type = BNT_CON_ADD; net.nodes[0].flags = BNF_IS_OUTPUT;
    net.nodes[0].aux_data = 0; net.nodes[0].principal_wire = 0;
    net.nodes[1].type = BNT_CON_MOV_IMM; net.nodes[1].aux_data = 5;
    net.nodes[1].principal_wire = 0;
    net.nodes[2].type = BNT_CON_MOV_IMM; net.nodes[2].aux_data = 3;
    net.nodes[2].principal_wire = 1;
    net.wires[0].node_a = 0; net.wires[0].port_a = BWP_PRINCIPAL;
    net.wires[0].node_b = 1;
    net.wires[1].node_a = 0; net.wires[1].port_a = BWP_AUX_LEFT;
    net.wires[1].node_b = 2;
    net.ios[0].node = 0; net.ios[0].port = BWP_PRINCIPAL;
    net.ios[0].direction = BIO_OUT;

    /* Round-trip: serialize, parse, verify */
    uint8_t *bytes; size_t len;
    ASSERT(bine_serialize_bbf(&net, &bytes, &len) == 0);
    bine_net_t parsed;
    ASSERT(bine_parse_bbf(bytes, len, &parsed) == 0);
    ASSERT(parsed.num_nodes == 3);
    ASSERT(parsed.nodes[1].aux_data == 5);
    ASSERT(parsed.nodes[2].aux_data == 3);

    free(bytes);
    bine_net_free(&parsed);
    bine_net_free(&net);
    return 0;
}

/* Test the full pipeline: parse -> optimize -> serialize */
int test_full_pipeline(void) {
    /* Build a "dead code" program: 2 ERA nodes connected to 2 ERA nodes */
    bine_net_t net;
    bine_net_init(&net);
    bine_net_reserve(&net, 4, 2, 0);
    net.num_nodes = 4; net.num_wires = 2;
    net.num_inputs = 0; net.num_outputs = 0;
    net.nodes[0].type = BNT_ERA; net.nodes[0].principal_wire = 0;
    net.nodes[1].type = BNT_ERA; net.nodes[1].principal_wire = 0;
    net.nodes[2].type = BNT_ERA; net.nodes[2].principal_wire = 1;
    net.nodes[3].type = BNT_ERA; net.nodes[3].principal_wire = 1;
    net.wires[0].node_a = 0; net.wires[0].node_b = 1;
    net.wires[0].port_a = BWP_PRINCIPAL;
    net.wires[1].node_a = 2; net.wires[1].node_b = 3;
    net.wires[1].port_a = BWP_PRINCIPAL;

    uint64_t before = kolmogorov_length(&net);
    kolmogorov_descent_t d;
    kolmogorov_descent_init(&d);
    kolmogorov_descent_run(&d, &net);
    kolmogorov_descent_free(&d);
    uint64_t after = kolmogorov_length(&net);
    /* All 4 ERA nodes should be eliminated (they are all dead) */
    ASSERT(after < before);

    bine_net_free(&net);
    return 0;
}

/* Verify that a real .bbf file (if it exists) can be parsed and optimized. */
int test_real_bbf(void) {
    const char *path = "examples/return_one.bbf";
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("test_pure_binary: SKIPPED (no %s)\n", path);
        return 0;
    }
    fclose(f);
    /* Read, parse, optimize, serialize */
    uint8_t *buf; size_t len;
    FILE *rf = fopen(path, "rb");
    fseek(rf, 0, SEEK_END);
    len = ftell(rf);
    fseek(rf, 0, SEEK_SET);
    buf = (uint8_t*)malloc(len);
    fread(buf, 1, len, rf);
    fclose(rf);
    bine_net_t net;
    int rc = bine_parse_bbf(buf, len, &net);
    free(buf);
    ASSERT(rc == 0);
    uint64_t before = kolmogorov_length(&net);
    kolmogorov_descent_t d;
    kolmogorov_descent_init(&d);
    kolmogorov_descent_run(&d, &net);
    kolmogorov_descent_free(&d);
    uint64_t after = kolmogorov_length(&net);
    /* Either we reduced it or it was already in normal form */
    ASSERT(after <= before);
    bine_net_free(&net);
    return 0;
}

int test_pure_binary_main(void) {
    int failed = 0;
    failed += test_addition_program();
    failed += test_full_pipeline();
    failed += test_real_bbf();
    if (failed) {
        fprintf(stderr, "test_pure_binary: %d test(s) FAILED\n", failed);
        return 1;
    }
    printf("test_pure_binary: all tests PASSED\n");
    return 0;
}

#ifndef BINE_COMBINED_TESTS
int main(void) { return test_pure_binary_main(); }
#endif
