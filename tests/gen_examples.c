/*
 * gen_examples.c - Generate hand-crafted .bbf example files
 *
 * Compile and run to produce:
 *   examples/return_one.bbf   - net that produces the value 1
 *   examples/return_eight.bbf - net that computes 5+3=8
 *   examples/dead_code.bbf    - net that has dead code (reduces to empty)
 *   examples/duplicator.bbf   - net that demonstrates DUP x DUP reduction
 */

#include "bbf.h"
#include "kolmogorov.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_net(const char *path, const bine_net_t *net) {
    uint8_t *out; size_t out_len;
    if (bine_serialize_bbf(net, &out, &out_len) < 0) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) { free(out); return -2; }
    fwrite(out, 1, out_len, f);
    fclose(f);
    free(out);
    return 0;
}

int main(void) {
    /* --- return_one.bbf: a single MOV_IMM with value 1 --- */
    {
        bine_net_t net;
        bine_net_init(&net);
        bine_net_reserve(&net, 1, 0, 1);
        net.num_nodes = 1; net.num_wires = 0;
        net.num_inputs = 0; net.num_outputs = 1;
        net.nodes[0].type = BNT_CON_MOV_IMM;
        net.nodes[0].flags = BNF_IS_OUTPUT;
        net.nodes[0].aux_data = 1;
        net.nodes[0].principal_wire = 0xFFFFFFFFu;
        net.ios[0].node = 0;
        net.ios[0].port = BWP_PRINCIPAL;
        net.ios[0].direction = BIO_OUT;
        if (write_net("examples/return_one.bbf", &net) == 0)
            printf("Wrote examples/return_one.bbf\n");
        bine_net_free(&net);
    }

    /* --- return_eight.bbf: 5 + 3 = 8 (uses ADD node) --- */
    {
        bine_net_t net;
        bine_net_init(&net);
        bine_net_reserve(&net, 3, 2, 1);
        net.num_nodes = 3; net.num_wires = 2;
        net.num_inputs = 0; net.num_outputs = 1;
        net.nodes[0].type = BNT_CON_ADD;
        net.nodes[0].flags = BNF_IS_OUTPUT;
        net.nodes[0].aux_data = 0;
        net.nodes[0].principal_wire = 0;
        net.nodes[1].type = BNT_CON_MOV_IMM;
        net.nodes[1].aux_data = 5;
        net.nodes[1].principal_wire = 0;
        net.nodes[2].type = BNT_CON_MOV_IMM;
        net.nodes[2].aux_data = 3;
        net.nodes[2].principal_wire = 1;
        net.wires[0].node_a = 0; net.wires[0].port_a = BWP_PRINCIPAL;
        net.wires[0].node_b = 1;
        net.wires[1].node_a = 0; net.wires[1].port_a = BWP_AUX_LEFT;
        net.wires[1].node_b = 2;
        net.ios[0].node = 0; net.ios[0].port = BWP_PRINCIPAL;
        net.ios[0].direction = BIO_OUT;
        if (write_net("examples/return_eight.bbf", &net) == 0)
            printf("Wrote examples/return_eight.bbf\n");
        bine_net_free(&net);
    }

    /* --- dead_code.bbf: two ERA x ERA pairs that should be eliminated --- */
    {
        bine_net_t net;
        bine_net_init(&net);
        bine_net_reserve(&net, 4, 2, 0);
        net.num_nodes = 4; net.num_wires = 2;
        net.num_inputs = 0; net.num_outputs = 0;
        for (int i = 0; i < 4; i++) {
            net.nodes[i].type = BNT_ERA;
            net.nodes[i].principal_wire = (uint32_t)(i / 2);
        }
        net.wires[0].node_a = 0; net.wires[0].node_b = 1;
        net.wires[0].port_a = BWP_PRINCIPAL;
        net.wires[1].node_a = 2; net.wires[1].node_b = 3;
        net.wires[1].port_a = BWP_PRINCIPAL;
        if (write_net("examples/dead_code.bbf", &net) == 0)
            printf("Wrote examples/dead_code.bbf (will reduce to empty)\n");
        bine_net_free(&net);
    }

    /* --- duplicator.bbf: DUP x DUP pair that should annihilate --- */
    {
        bine_net_t net;
        bine_net_init(&net);
        bine_net_reserve(&net, 2, 1, 0);
        net.num_nodes = 2; net.num_wires = 1;
        net.num_inputs = 0; net.num_outputs = 0;
        net.nodes[0].type = BNT_DUP;
        net.nodes[0].principal_wire = 0;
        net.nodes[1].type = BNT_DUP;
        net.nodes[1].principal_wire = 0;
        net.wires[0].node_a = 0; net.wires[0].node_b = 1;
        net.wires[0].port_a = BWP_PRINCIPAL;
        if (write_net("examples/duplicator.bbf", &net) == 0)
            printf("Wrote examples/duplicator.bbf (will reduce)\n");
        bine_net_free(&net);
    }

    /* --- hello.bbf: outputs "Hello" via syscall chain --- */
    {
        bine_net_t net;
        bine_net_init(&net);
        bine_net_reserve(&net, 8, 4, 1);
        net.num_nodes = 8; net.num_wires = 4;
        net.num_inputs = 0; net.num_outputs = 1;
        /* SYSCALL node for write(1, "Hello", 5) */
        net.nodes[0].type = BNT_CON_SYSCALL;
        net.nodes[0].flags = BNF_IS_OUTPUT;
        net.nodes[0].aux_data = 1;  /* SYS_write */
        net.nodes[0].principal_wire = 0;
        net.nodes[1].type = BNT_CON_MOV_IMM; net.nodes[1].aux_data = 1;
        net.nodes[1].principal_wire = 0;  /* fd = 1 (stdout) */
        net.nodes[2].type = BNT_CON_MOV_IMM; net.nodes[2].aux_data = 0x6C6C6548;  /* "Hell" little-endian */
        net.nodes[2].principal_wire = 1;  /* buf = "Hell" */
        net.nodes[3].type = BNT_CON_MOV_IMM; net.nodes[3].aux_data = 5;
        net.nodes[3].principal_wire = 2;  /* count = 5 */
        net.nodes[4].type = BNT_CON_MOV_IMM; net.nodes[4].aux_data = 0x000A6F;  /* "o\n" */
        net.nodes[4].principal_wire = 3;
        net.wires[0].node_a = 0; net.wires[0].node_b = 1;
        net.wires[0].port_a = BWP_PRINCIPAL;
        net.wires[1].node_a = 0; net.wires[1].node_b = 2;
        net.wires[1].port_a = BWP_AUX_LEFT;
        net.wires[2].node_a = 0; net.wires[2].node_b = 3;
        net.wires[2].port_a = BWP_AUX_RIGHT;
        net.wires[3].node_a = 0; net.wires[3].node_b = 4;
        net.wires[3].port_a = BWP_AUX_RIGHT;
        net.ios[0].node = 0; net.ios[0].port = BWP_PRINCIPAL;
        net.ios[0].direction = BIO_OUT;
        if (write_net("examples/hello.bbf", &net) == 0)
            printf("Wrote examples/hello.bbf\n");
        bine_net_free(&net);
    }

    /* --- halt.bbf: a HALT node (terminates immediately) --- */
    {
        bine_net_t net;
        bine_net_init(&net);
        bine_net_reserve(&net, 1, 0, 0);
        net.num_nodes = 1; net.num_wires = 0;
        net.num_inputs = 0; net.num_outputs = 0;
        net.nodes[0].type = BNT_CON_HALT;
        net.nodes[0].principal_wire = 0xFFFFFFFFu;
        if (write_net("examples/halt.bbf", &net) == 0)
            printf("Wrote examples/halt.bbf\n");
        bine_net_free(&net);
    }

    return 0;
}
