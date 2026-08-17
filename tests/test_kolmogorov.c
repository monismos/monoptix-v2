/*
 * test_kolmogorov.c - Tests for Kolmogorov Lattice
 */

#include "kolmogorov.h"
#include "bbf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int test_length_metric(void) {
    bine_net_t net;
    bine_net_init(&net);
    bine_net_reserve(&net, 5, 3, 1);
    net.num_nodes = 5; net.num_wires = 3;
    net.num_inputs = 0; net.num_outputs = 1;
    uint64_t len = kolmogorov_length(&net);
    ASSERT(len == 5 + 3 + 1);
    bine_net_free(&net);
    return 0;
}

int test_empty_normal_form(void) {
    bine_net_t net;
    bine_net_init(&net);
    bine_net_reserve(&net, 0, 0, 0);
    ASSERT(kolmogorov_is_normal_form(&net));
    ASSERT(kolmogorov_count_active_pairs(&net) == 0);
    bine_net_free(&net);
    return 0;
}

int test_active_pair_detection(void) {
    bine_net_t net;
    bine_net_init(&net);
    bine_net_reserve(&net, 2, 1, 0);
    net.num_nodes = 2; net.num_wires = 1;
    net.nodes[0].type = BNT_ERA;
    net.nodes[0].principal_wire = 0;
    net.nodes[1].type = BNT_ERA;
    net.nodes[1].principal_wire = 0;
    net.wires[0].node_a = 0;
    net.wires[0].port_a = BWP_PRINCIPAL;
    net.wires[0].node_b = 1;
    ASSERT(kolmogorov_count_active_pairs(&net) == 1);
    ASSERT(!kolmogorov_is_normal_form(&net));
    bine_net_free(&net);
    return 0;
}

int test_era_era_reduction(void) {
    bine_net_t net;
    bine_net_init(&net);
    bine_net_reserve(&net, 2, 1, 0);
    net.num_nodes = 2; net.num_wires = 1;
    net.nodes[0].type = BNT_ERA; net.nodes[0].principal_wire = 0;
    net.nodes[1].type = BNT_ERA; net.nodes[1].principal_wire = 0;
    net.wires[0].node_a = 0; net.wires[0].node_b = 1;
    net.wires[0].port_a = BWP_PRINCIPAL;
    int rc = kolmogorov_step(&net);
    ASSERT(rc == 1);
    /* Both nodes marked dead, wire marked dead */
    ASSERT(net.nodes[0].type == 0xFF);
    ASSERT(net.nodes[1].type == 0xFF);
    bine_net_free(&net);
    return 0;
}

int test_con_era_reduction(void) {
    bine_net_t net;
    bine_net_init(&net);
    bine_net_reserve(&net, 2, 1, 0);
    net.num_nodes = 2; net.num_wires = 1;
    net.nodes[0].type = BNT_CON_ADD; net.nodes[0].principal_wire = 0;
    net.nodes[1].type = BNT_ERA;      net.nodes[1].principal_wire = 0;
    net.wires[0].node_a = 0; net.wires[0].node_b = 1;
    net.wires[0].port_a = BWP_PRINCIPAL;
    ASSERT(kolmogorov_step(&net) == 1);
    ASSERT(net.nodes[0].type == 0xFF);
    bine_net_free(&net);
    return 0;
}

int test_descent_runs(void) {
    bine_net_t net;
    bine_net_init(&net);
    bine_net_reserve(&net, 4, 2, 0);
    net.num_nodes = 4; net.num_wires = 2;
    /* Pair 1: ERA + ERA (reduces) */
    net.nodes[0].type = BNT_ERA; net.nodes[0].principal_wire = 0;
    net.nodes[1].type = BNT_ERA; net.nodes[1].principal_wire = 0;
    net.wires[0].node_a = 0; net.wires[0].node_b = 1;
    net.wires[0].port_a = BWP_PRINCIPAL;
    /* Pair 2: CON + ERA (reduces) */
    net.nodes[2].type = BNT_CON_MUL; net.nodes[2].principal_wire = 1;
    net.nodes[3].type = BNT_ERA;     net.nodes[3].principal_wire = 1;
    net.wires[1].node_a = 2; net.wires[1].node_b = 3;
    net.wires[1].port_a = BWP_PRINCIPAL;
    uint64_t before = kolmogorov_length(&net);
    kolmogorov_descent_t d;
    kolmogorov_descent_init(&d);
    int rounds = kolmogorov_descent_run(&d, &net);
    kolmogorov_descent_free(&d);
    uint64_t after = kolmogorov_length(&net);
    ASSERT(after < before);
    ASSERT(rounds >= 1);
    bine_net_free(&net);
    return 0;
}

int test_bound_compute(void) {
    bine_net_t net;
    bine_net_init(&net);
    bine_net_reserve(&net, 10, 5, 2);
    net.num_nodes = 10; net.num_wires = 5;
    net.num_inputs = 1; net.num_outputs = 1;
    kolmogorov_bound_t b;
    kolmogorov_bound_compute(&net, 8, &b);
    ASSERT(b.lower_bound == 8);
    ASSERT(b.upper_bound == 10 + 5 + 2);
    ASSERT(b.ratio > 1.0);
    bine_net_free(&net);
    return 0;
}

int test_kolmogorov_main(void) {
    int failed = 0;
    failed += test_length_metric();
    failed += test_empty_normal_form();
    failed += test_active_pair_detection();
    failed += test_era_era_reduction();
    failed += test_con_era_reduction();
    failed += test_descent_runs();
    failed += test_bound_compute();
    if (failed) {
        fprintf(stderr, "test_kolmogorov: %d test(s) FAILED\n", failed);
        return 1;
    }
    printf("test_kolmogorov: all tests PASSED\n");
    return 0;
}

#ifndef BINE_COMBINED_TESTS
int main(void) { return test_kolmogorov_main(); }
#endif
