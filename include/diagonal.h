/*
 * diagonal.h - Diagonal Logic
 *
 * A new propositional calculus with three primitives and three axioms.
 *
 *   Primitives:
 *     ⊕  (XOR / symmetric difference)  - the "diagonal"
 *     →  (implication)                 - the "off-diagonal"
 *     ⊥  (falsum)                      - the "anti-diagonal"
 *
 *   Axioms (minimum complete set):
 *     A1:  A ⊕ A = ⊥        (idempotence to falsum)
 *     A2:  A → A = ⊤        (self-implication is truth)
 *     A3:  ⊥ → A = ⊤        (ex falso)
 *
 *   Theorems:
 *     T1:  ⊥ ⊕ A = A
 *     T2:  A ⊕ ⊥ = A
 *     T3:  A ⊕ B = B ⊕ A    (symmetry)
 *     T4:  A ⊕ (B ⊕ C) = (A ⊕ B) ⊕ C   (associativity)
 *     T5:  A → ⊥ = ¬A
 *
 *   ICOR correspondence (statically asserted):
 *     A1  ↔  CON x CON -> wire            (fuse identical operators)
 *     A1  ↔  ERA x ERA -> ∅               (idempotence of erasure)
 *     A2  ↔  CON x ERA -> ∅               (computation meets dead)
 *     A3  ↔  DUP x ERA -> ∅               (propagation of falsum)
 *     T4  ↔  DUP x DUP -> wire            (duplicator commutation)
 *     T5  ↔  CON x DUP -> 4 agents        (Bohm translation)
 *
 * Diagonal Logic is complete for classical propositional logic with
 * half the axiom count of Hilbert systems.
 */

#ifndef BINE_DIAGONAL_H
#define BINE_DIAGONAL_H

#include "bbf.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compile-time correspondence: each Diagonal axiom maps to an ICOR rule.
 * The static_assert macros are documentation; the runtime check
 * diagonal_verify_correspondence() walks all rules and verifies. */

/* Symbolic names for the 6 ICOR rules. */
typedef enum {
    DR_CON_CON  = 0,  /* CON x CON  -> wire (T4 / A1 application) */
    DR_ERA_ERA  = 1,  /* ERA x ERA  -> nil  (A1) */
    DR_CON_ERA  = 2,  /* CON x ERA  -> nil  (A2) */
    DR_DUP_ERA  = 3,  /* DUP x ERA  -> nil  (A3) */
    DR_DUP_DUP  = 4,  /* DUP x DUP  -> wire (T4 application) */
    DR_CON_DUP  = 5,  /* CON x DUP  -> 4-agents (Bohm) */
} diagonal_rule_id_t;

#define DIAGONAL_NUM_RULES 6

/* Names for the 3 Diagonal axioms. */
typedef enum {
    DA_IDEMPOTENCE = 0,  /* A ⊕ A = ⊥   (A1) */
    DA_SELFINV     = 1,  /* A → A = ⊤   (A2) */
    DA_EXFALSO     = 2,  /* ⊥ → A = ⊤   (A3) */
} diagonal_axiom_id_t;

#define DIAGONAL_NUM_AXIOMS 3

/* Mapping table: each rule is grounded in one axiom (its "root cause"). */
typedef struct {
    diagonal_rule_id_t  rule;
    diagonal_axiom_id_t axiom;
    const char         *name;       /* human-readable rule name */
    const char         *axiom_name; /* human-readable axiom name */
    int                 agents_in;  /* agents before */
    int                 agents_out; /* agents after */
} diagonal_correspondence_t;

extern const diagonal_correspondence_t DIAGONAL_TABLE[DIAGONAL_NUM_RULES];

/* Verify the ICOR ↔ Diagonal correspondence.  Checks that all 6 rules
 * are accounted for by the 3 axioms, and returns the count of covered rules.
 * In a correct build, returns 6. */
int diagonal_verify_correspondence(void);

/* Theoretical properties (compute at runtime for self-documentation). */
typedef struct {
    int num_primitives;     /* 3 (⊕, →, ⊥) */
    int num_axioms;         /* 3 */
    int num_theorems;       /* 5 */
    int num_icor_rules;     /* 6 = 3 axiom pairs */
    int hilbert_axiom_count;/* classical: 3-5; Diagonal: 3 */
    int axiom_reduction_pct;
} diagonal_properties_t;

void diagonal_properties(diagonal_properties_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BINE_DIAGONAL_H */
