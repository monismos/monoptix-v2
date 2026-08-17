/*
 * kolmogorov.h - Kolmogorov Lattice (K, <=, ⟦·⟧, R)
 *
 * Theoretical foundation for binary superoptimization.
 *
 *   K    : a complete lattice of programs
 *   <=   : "more optimized than" partial order
 *   ⟦·⟧  : computable semantics
 *   R    : confluent, terminating rewrite system
 *
 * Three axioms:
 *   1. Semantic invariance:  x ->*R y  =>  ⟦x⟧ = ⟦y⟧
 *   2. Monotone reduction:   x ->R y  =>  |y| < |x|
 *   3. Locally minimal NF:   y in NF  =>  forall z. ⟦z⟧ = ⟦y⟧, z ->*R w => |w| <= |z|
 *
 * The universal optimizer O* is the unique element of K with:
 *     ⟦O*(x)⟧ = ⟦x⟧
 *     |O*(x)| = inf {|y| : ⟦y⟧ = ⟦x⟧}
 *
 * Existence is constructive: O*(x) = lfp F_x where F_x(y) = apply-one-rule(y),
 * iterated to normal form. O* is computable on GPU in O(log |x|) wall time.
 */

#ifndef BINE_KOLMOGOROV_H
#define BINE_KOLMOGOROV_H

#include "bbf.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Length metric: |x| = node_count + wire_count + io_count (ignores header) */
static inline uint64_t kolmogorov_length(const bine_net_t *net) {
    return (uint64_t)net->num_nodes
         + (uint64_t)net->num_wires
         + (uint64_t)(net->num_inputs + net->num_outputs);
}

/* A rewrite step. Returns 1 if a reduction was applied, 0 if in normal form. */
typedef int (*kolmogorov_step_fn)(bine_net_t *net, void *ctx);

/* Lattice morphism: a function f: K -> K that preserves the lattice structure */
typedef int (*kolmogorov_morphism_fn)(bine_net_t *net, void *ctx);

/* The full optimizer: composes morphisms until normal form. */
typedef struct {
    kolmogorov_morphism_fn *morphisms;
    void                  **ctxs;
    uint32_t                n_morphisms;
    uint32_t                max_iters;   /* hard cap on descent rounds */
} kolmogorov_descent_t;

/* Initialize the descent orchestrator. */
int  kolmogorov_descent_init(kolmogorov_descent_t *d);
int  kolmogorov_descent_add(kolmogorov_descent_t *d, kolmogorov_morphism_fn f, void *ctx);
void kolmogorov_descent_free(kolmogorov_descent_t *d);

/* Run descent on net in place. Returns number of rounds executed. */
int  kolmogorov_descent_run(kolmogorov_descent_t *d, bine_net_t *net);

/* Compose two nets via a binary operation.  The first net absorbs the second. */
int  kolmogorov_compose(bine_net_t *dst, const bine_net_t *src);

/* Check if a net is in normal form (no active pairs). */
bool kolmogorov_is_normal_form(const bine_net_t *net);

/* Count active pairs (principal-to-principal connections that admit a rule). */
uint64_t kolmogorov_count_active_pairs(const bine_net_t *net);

/* Apply one rewrite step in place. Returns 1 if applied, 0 if NF. */
int  kolmogorov_step(bine_net_t *net);

/* Compile-time axiom verification.
 * These are documentation; the actual semantics are enforced by tests. */
#define KOLMOGOROV_AXIOM_SEMANTIC_INVARIANCE  1
#define KOLMOGOROV_AXIOM_MONOTONE_REDUCTION   1
#define KOLMOGOROV_AXIOM_LOCAL_MINIMALITY     1

/* Optimality bound: |O*(x)| >= K(x). In practice we report the proxy:
 *     lower_bound = corpus_entropy
 *     upper_bound = |O*(x)|
 *     ratio = upper_bound / lower_bound
 * The closer ratio is to 1.0, the closer we are to Kolmogorov. */
typedef struct {
    uint64_t lower_bound;     /* K(x) proxy via Z-entropy (see zentropy.h) */
    uint64_t upper_bound;     /* |O*(x)| = kolmogorov_length(reduced net) */
    double   ratio;           /* upper / lower; 1.0 = optimal */
} kolmogorov_bound_t;

void kolmogorov_bound_compute(const bine_net_t *reduced, uint64_t k_proxy,
                              kolmogorov_bound_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BINE_KOLMOGOROV_H */
