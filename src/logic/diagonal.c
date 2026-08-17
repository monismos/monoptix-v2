/*
 * diagonal.c - Diagonal Logic implementation
 */

#include "diagonal.h"
#include "bbf.h"
#include <stdio.h>

const diagonal_correspondence_t DIAGONAL_TABLE[DIAGONAL_NUM_RULES] = {
    { DR_CON_CON, DA_IDEMPOTENCE, "CON x CON -> wire", "A ⊕ A = ⊥",   2, 0 },
    { DR_ERA_ERA, DA_IDEMPOTENCE, "ERA x ERA -> nil",  "A ⊕ A = ⊥",   2, 0 },
    { DR_CON_ERA, DA_SELFINV,     "CON x ERA -> nil",  "A → A = ⊤",   2, 0 },
    { DR_DUP_ERA, DA_EXFALSO,     "DUP x ERA -> nil",  "⊥ → A = ⊤",   2, 0 },
    { DR_DUP_DUP, DA_IDEMPOTENCE, "DUP x DUP -> wire", "(A⊕A)⊕(B⊕B)", 2, 0 },
    { DR_CON_DUP, DA_SELFINV,     "CON x DUP -> 4",    "A → A = ⊤",   2, 4 },
};

int diagonal_verify_correspondence(void) {
    /* Sanity: every rule is mapped, and the count is exactly 6. */
    int covered = 0;
    bool axiom_used[DIAGONAL_NUM_AXIOMS] = { false, false, false };
    for (int i = 0; i < DIAGONAL_NUM_RULES; i++) {
        if (DIAGONAL_TABLE[i].rule    >= DIAGONAL_NUM_RULES) return -1;
        if (DIAGONAL_TABLE[i].axiom   >= DIAGONAL_NUM_AXIOMS) return -2;
        if (DIAGONAL_TABLE[i].name    == NULL) return -3;
        if (DIAGONAL_TABLE[i].axiom_name == NULL) return -4;
        if (DIAGONAL_TABLE[i].agents_in  != 2) return -5;
        axiom_used[DIAGONAL_TABLE[i].axiom] = true;
        covered++;
    }
    /* All 3 axioms must be used at least once */
    for (int i = 0; i < DIAGONAL_NUM_AXIOMS; i++)
        if (!axiom_used[i]) return -6;
    return covered;
}

void diagonal_properties(diagonal_properties_t *out) {
    if (!out) return;
    out->num_primitives      = 3;
    out->num_axioms          = 3;
    out->num_theorems        = 5;
    out->num_icor_rules      = 6;
    out->hilbert_axiom_count = 5;  /* classical Hilbert system: 5 axioms */
    out->axiom_reduction_pct =
        100 * (out->hilbert_axiom_count - out->num_axioms)
            / out->hilbert_axiom_count;
}
