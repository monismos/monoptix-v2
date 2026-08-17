/*
 * gpu_runner.cu - Execute a reduced net on the GPU
 *
 * The reduced net is the optimized program.  Each node = one CUDA thread.
 * Wires are values flowing through nodes; we compute them step by step
 * (each step is a barrier) until the HALT node fires.
 */

#include "gpu_runner.h"
#include "bbf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __CUDACC__
#include <cuda_runtime.h>

/* Per-thread state: holds the value(s) computed by this node. */
typedef struct {
    int      step;          /* current step number */
    uint64_t value;         /* result of this node */
    int      fired;         /* has this node produced a value yet? */
} gpu_node_state_t;

__device__ uint64_t gpu_apply_op(uint8_t type, uint16_t aux,
                                  uint64_t a, uint64_t b) {
    switch (type) {
        case BNT_CON_NAND: return ~(a & b);
        case BNT_CON_AND:  return a & b;
        case BNT_CON_OR:   return a | b;
        case BNT_CON_XOR:  return a ^ b;
        case BNT_CON_NOT:  return ~a;
        case BNT_CON_ADD:  return a + b;
        case BNT_CON_SUB:  return a - b;
        case BNT_CON_MUL:  return a * b;
        case BNT_CON_DIV:  return b ? a / b : 0;
        case BNT_CON_MOD:  return b ? a % b : 0;
        case BNT_CON_SHL:  return a << (b & 63);
        case BNT_CON_SHR:  return a >> (b & 63);
        case BNT_CON_CMP_EQ: return a == b;
        case BNT_CON_CMP_LT: return a < b;
        case BNT_CON_CMP_GT: return a > b;
        case BNT_CON_MOV:    return a;
        case BNT_CON_MOV_IMM: return (uint64_t)(int64_t)(int16_t)aux;
        case BNT_CON_BIT_AND: return a & b;
        case BNT_CON_BIT_OR:  return a | b;
        case BNT_CON_BIT_XOR: return a ^ b;
        case BNT_CON_BIT_NOT: return ~a;
        default: return 0;
    }
}

extern "C" __global__ void gpu_runner_kernel(
    const bbf_node_t *nodes,
    const bbf_wire_t *wires,
    int n_nodes, int n_wires,
    gpu_node_state_t *states,
    gpu_runner_state_t *runner,
    int max_steps)
{
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= n_nodes) return;

    bbf_node_t n = nodes[tid];
    /* For each step: read inputs from connected nodes, compute, write back. */
    for (int step = 0; step < max_steps; step++) {
        if (runner->halted) return;
        /* Find all wires where this node is an endpoint, accumulate values */
        uint64_t a = 0, b = 0;
        int a_set = 0, b_set = 0;
        for (int w = 0; w < n_wires; w++) {
            bbf_wire_t wire = wires[w];
            if (wire.node_a == tid) {
                int other = wire.node_b;
                if (other < n_nodes && states[other].fired) {
                    if (!a_set) { a = states[other].value; a_set = 1; }
                    else if (!b_set) { b = states[other].value; b_set = 1; }
                }
            }
            if (wire.node_b == tid) {
                int other = wire.node_a;
                if (other < n_nodes && states[other].fired) {
                    if (!a_set) { a = states[other].value; a_set = 1; }
                    else if (!b_set) { b = states[other].value; b_set = 1; }
                }
            }
        }
        /* Compute */
        uint64_t result = 0;
        if (n.type == BNT_ERA) {
            /* Eraser: result is 0, marks "consumed" */
            result = 0;
        } else if (n.type == BNT_DUP) {
            /* Duplicator: forwards a to all connected */
            result = a;
        } else if (n.type == BNT_CON_HALT) {
            runner->halted = 1;
            return;
        } else if (n.type == BNT_CON_SYSCALL) {
            /* Request a syscall */
            runner->syscall_nr = n.aux_data;
            runner->syscall_pending = 1;
            result = 0;
        } else if (n.type >= BNT_CON_NAND && n.type <= BNT_CON_BIT_NOT) {
            result = gpu_apply_op(n.type, n.aux_data, a, b);
        } else {
            result = a;
        }
        states[tid].value = result;
        states[tid].fired = 1;
        states[tid].step = step;
        __syncthreads();
    }
}

#endif /* __CUDACC__ */

int gpu_runner_state_init(gpu_runner_state_t *s, const bine_net_t *net,
                            size_t memory_size) {
    if (!s || !net) return -1;
    memset(s, 0, sizeof(*s));
    s->num_inputs = net->num_inputs;
    s->num_outputs = net->num_outputs;
    if (s->num_inputs)
        s->input_values = (uint64_t*)calloc(s->num_inputs, sizeof(uint64_t));
    if (s->num_outputs)
        s->output_values = (uint64_t*)calloc(s->num_outputs, sizeof(uint64_t));
    s->memory_size = memory_size;
    if (memory_size) s->memory = (uint8_t*)calloc(memory_size, 1);
    return 0;
}

void gpu_runner_state_free(gpu_runner_state_t *s) {
    if (!s) return;
    free(s->input_values);  s->input_values = NULL;
    free(s->output_values); s->output_values = NULL;
    free(s->memory);        s->memory = NULL;
    s->num_inputs = s->num_outputs = 0;
}

int gpu_runner_execute(const bine_net_t *net, gpu_runner_state_t *state) {
    if (!net || !state) return -1;
#ifdef __CUDACC__
    if (net->num_nodes == 0) return 0;
    bbf_node_t *d_nodes = NULL;
    bbf_wire_t *d_wires = NULL;
    gpu_node_state_t *d_states = NULL;
    gpu_runner_state_t *d_runner = NULL;
    cudaMalloc(&d_nodes,  net->num_nodes * sizeof(bbf_node_t));
    cudaMalloc(&d_wires,  net->num_wires * sizeof(bbf_wire_t));
    cudaMalloc(&d_states, net->num_nodes * sizeof(gpu_node_state_t));
    /* Use pinned host memory for the runner state so CPU can poll it */
    cudaMallocHost((void**)&d_runner, sizeof(gpu_runner_state_t));
    *d_runner = *state;
    cudaMemcpy(d_nodes, net->nodes, net->num_nodes * sizeof(bbf_node_t), cudaMemcpyHostToDevice);
    cudaMemcpy(d_wires, net->wires, net->num_wires * sizeof(bbf_wire_t), cudaMemcpyHostToDevice);
    cudaMemset(d_states, 0, net->num_nodes * sizeof(gpu_node_state_t));
    int threads = 256;
    int blocks  = (net->num_nodes + threads - 1) / threads;
    if (blocks > 65535) blocks = 65535;
    gpu_runner_kernel<<<blocks, threads>>>(
        d_nodes, d_wires, (int)net->num_nodes, (int)net->num_wires,
        d_states, d_runner, 1024);
    cudaDeviceSynchronize();
    /* Copy back */
    *state = *d_runner;
    cudaFree(d_nodes);
    cudaFree(d_wires);
    cudaFree(d_states);
    cudaFreeHost(d_runner);
    return 0;
#else
    /* CPU fallback: a trivial walk over the net. */
    if (net->num_nodes == 0) return 0;
    /* Compute BFS order by following principal wires */
    uint64_t *values = (uint64_t*)calloc(net->num_nodes, sizeof(uint64_t));
    int *fired = (int*)calloc(net->num_nodes, sizeof(int));
    for (int step = 0; step < 1024; step++) {
        if (state->halted) break;
        int progress = 0;
        for (uint32_t i = 0; i < net->num_nodes; i++) {
            bbf_node_t n = net->nodes[i];
            if (n.type == BNT_ERA || (n.type >= BNT_CON_NAND && n.type <= BNT_CON_BIT_NOT)) {
                uint64_t a = 0, b = 0;
                int a_set = 0, b_set = 0;
                for (uint32_t w = 0; w < net->num_wires; w++) {
                    const bbf_wire_t *wire = &net->wires[w];
                    int other = -1;
                    if (wire->node_a == i) other = (int)wire->node_b;
                    else if (wire->node_b == i) other = (int)wire->node_a;
                    if (other >= 0 && fired[other]) {
                        if (!a_set) { a = values[other]; a_set = 1; }
                        else if (!b_set) { b = values[other]; b_set = 1; }
                    }
                }
                if (n.type == BNT_ERA) values[i] = 0;
                else if (n.type == BNT_CON_HALT) { state->halted = 1; }
                else if (n.type == BNT_CON_SYSCALL) {
                    state->syscall_nr = n.aux_data;
                    state->syscall_pending = 1;
                } else {
                    switch (n.type) {
                        case BNT_CON_ADD: values[i] = a + b; break;
                        case BNT_CON_SUB: values[i] = a - b; break;
                        case BNT_CON_MUL: values[i] = a * b; break;
                        case BNT_CON_AND: values[i] = a & b; break;
                        case BNT_CON_OR:  values[i] = a | b; break;
                        case BNT_CON_XOR: values[i] = a ^ b; break;
                        case BNT_CON_CMP_EQ: values[i] = (a == b); break;
                        case BNT_CON_CMP_LT: values[i] = (a < b); break;
                        case BNT_CON_CMP_GT: values[i] = (a > b); break;
                        case BNT_CON_SHL: values[i] = a << (b & 63); break;
                        case BNT_CON_SHR: values[i] = a >> (b & 63); break;
                        case BNT_CON_MOV: case BNT_CON_MOV_IMM: values[i] = a; break;
                        default: values[i] = a; break;
                    }
                }
                fired[i] = 1;
                progress = 1;
            } else if (n.type == BNT_DUP) {
                /* Forward: copy from any fired neighbor */
                for (uint32_t w = 0; w < net->num_wires; w++) {
                    const bbf_wire_t *wire = &net->wires[w];
                    int other = -1;
                    if (wire->node_a == i) other = (int)wire->node_b;
                    else if (wire->node_b == i) other = (int)wire->node_a;
                    if (other >= 0 && fired[other]) {
                        values[i] = values[other];
                        fired[i] = 1;
                        progress = 1;
                        break;
                    }
                }
            }
        }
        if (!progress) break;
    }
    free(values);
    free(fired);
    return 0;
#endif
}
