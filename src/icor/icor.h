#ifndef ICOR_H
#define ICOR_H

#include "bine.h"
#include "bits.cuh"

#ifdef __cplusplus
extern "C" {
#endif

#define ICOR_MAX_AGENTS 1048576
#define ICOR_MAX_ROUNDS 64
#define ICOR_PAIR_CAPACITY (ICOR_MAX_AGENTS / 2)

typedef enum {
    ICOR_CON_CON = 0,
    ICOR_DUP_DUP = 1,
    ICOR_ERA_ERA = 2,
    ICOR_CON_DUP = 3,
    ICOR_CON_ERA = 4,
    ICOR_DUP_ERA = 5
} IcorRule;

typedef struct {
    uint32_t agent_a;
    uint32_t agent_b;
    uint32_t rule_type;
} IcorPair;

typedef struct {
    uint64_t* net;
    uint32_t n_agents;
    uint32_t capacity;
    uint32_t n_rounds;
    uint64_t n_pairs_found;
    uint64_t n_pairs_applied;
} IcorState;

IcorState* icor_create(uint32_t capacity);
void icor_destroy(IcorState* state);
int icor_load_net(IcorState* state, const uint64_t* net, uint32_t n_agents);
int icor_reduce(IcorState* state, uint32_t max_rounds);
uint64_t* icor_get_result(const IcorState* state, uint32_t* n_out);

#ifdef __cplusplus
}
#endif

#endif
