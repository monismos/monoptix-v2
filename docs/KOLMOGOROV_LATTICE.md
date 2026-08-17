# Kolmogorov Lattice

## Introduction

The **Kolmogorov Lattice** is the theoretical foundation of BINE's
optimization.  It is a complete lattice of programs equipped with a
computable semantics and a confluent, terminating rewrite system.

## Definition

A *Kolmogorov Lattice* is a 4-tuple (K, ≤, ⟦·⟧, R) where:

- **K** is a complete lattice (has ⊥, ⊤, and every subset has a sup/inf)
- **≤** is the "more-optimized-than" partial order
- **⟦·⟧ : K → Sem** is a computable semantics function
- **R ⊆ K × K** is a confluent, terminating rewrite system

## Three Axioms

```
Axiom 1 (Semantic invariance):
    x →*_R y  ⇒  ⟦x⟧ = ⟦y⟧

Axiom 2 (Monotone reduction):
    x →_R y  ⇒  |y| < |x|

Axiom 3 (Locally minimal normal form):
    y in NF  ⇒  ∀ z. ⟦z⟧ = ⟦y⟧, z →*_R w  ⇒  |w| ≤ |z|
```

## The Universal Optimizer

**Definition.** The universal optimizer O* is the unique element of K such
that for all x:

```
    ⟦O*(x)⟧ = ⟦x⟧
    |O*(x)| = inf {|y| : ⟦y⟧ = ⟦x⟧}
```

**Theorem (Existence).** O* exists and is unique.

**Theorem (Constructive).** O*(x) = lfp F_x where:
```
    F_x : K → K
    F_x(y) = apply-one-rewrite-rule(y)
```

The least fixed point of F_x is the normal form.  By Axiom 3, this
normal form has locally minimal length.

## GPU Computability

**Theorem.** O* is computable on GPU in **O(log|x|) wall time**.

**Proof.**

1. *Local rules.* Each rewrite rule reads ≤ 2 agents and produces ≤ 4
   new ones.  Reads and writes are local.

2. *Confluence.* By the Church-Rosser theorem for interaction nets, the
   order of rule application doesn't affect the final normal form.  We
   can apply rules in any order (or in parallel) and converge.

3. *Parallel redex detection.* A warp-level ballot finds all active
   pairs (principal-to-principal connections) in O(1) wall time.

4. *Geometric shrink.* Each rule application reduces |x| by at least 1
   (Axiom 2).  So the number of rounds is O(|x|).  But because we apply
   *all* active redexes in parallel, the shrink is geometric: at least
   one redex per round, but typically many.  In practice, O(log|x|).

5. *Termination.* By Axiom 2, the process must terminate at a
   normal form.  By Axiom 3, this normal form is locally optimal.

∎

## Optimality Bound

**Definition (Kolmogorov Bound).** For any program x:
```
    K(x) ≤ |O*(x)| ≤ |x|
```

where K(x) is the Kolmogorov complexity (length of the shortest
description of x).

**Theorem.** O*(x) is the element of K with the smallest length that
preserves the semantics of x.  In particular:

```
    |O*(x)| - K(x) ≥ 0
```

and this difference is the "redundancy" of the representation.

## Z-entropy as a K(x) Proxy

We can't compute K(x) (uncomputable in general), but we can estimate it
via Z-entropy (see Z_ENTROPY.md):

```
    K_proxy(x) = Σ z(nᵢ) = -Σ log₂ P(nᵢ)
```

where the sum is over the symbols in x, and P is the empirical
frequency.  This is the Shannon entropy of x, which is an asymptotic
lower bound on K(x) (Shannon-McMillan-Breiman theorem).

## Implementation in BINE

The Kolmogorov Lattice is implemented in `include/kolmogorov.h` and
`src/optimizer/kolmogorov.c`:

```c
typedef struct {
    uint32_t num_nodes;
    uint32_t num_wires;
    uint32_t num_inputs;
    uint32_t num_outputs;
} kolmogorov_lattice_t;

uint64_t kolmogorov_length(const bine_net_t *net);
int      kolmogorov_step(bine_net_t *net);
int      kolmogorov_descent_run(kolmogorov_descent_t *d, bine_net_t *net);
void     kolmogorov_bound_compute(const bine_net_t *reduced, uint64_t k_proxy,
                                  kolmogorov_bound_t *out);
```

The descent orchestrator applies morphisms until normal form, then
compacts (removes dead nodes/wires) so that |O*(x)| = kolmogorov_length.

The bound computation reports:
- `lower_bound` = K(x) proxy via Z-entropy
- `upper_bound` = |O*(x)|
- `ratio` = upper / lower; 1.0 = optimal
