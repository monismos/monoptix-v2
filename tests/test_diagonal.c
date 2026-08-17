/*
 * test_diagonal.c - Tests for Diagonal Logic
 */

#include "diagonal.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int test_correspondence_complete(void) {
    int covered = diagonal_verify_correspondence();
    ASSERT(covered == DIAGONAL_NUM_RULES);
    ASSERT(covered == 6);
    return 0;
}

int test_table_consistency(void) {
    /* All 6 rules must be unique */
    for (int i = 0; i < DIAGONAL_NUM_RULES; i++) {
        for (int j = i + 1; j < DIAGONAL_NUM_RULES; j++) {
            if (DIAGONAL_TABLE[i].rule == DIAGONAL_TABLE[j].rule) return 1;
        }
    }
    /* All 3 axioms must be used */
    bool used[3] = {false, false, false};
    for (int i = 0; i < DIAGONAL_NUM_RULES; i++) {
        used[DIAGONAL_TABLE[i].axiom] = true;
    }
    for (int i = 0; i < 3; i++) ASSERT(used[i]);
    return 0;
}

int test_properties(void) {
    diagonal_properties_t p;
    diagonal_properties(&p);
    ASSERT(p.num_primitives == 3);
    ASSERT(p.num_axioms == 3);
    ASSERT(p.num_icor_rules == 6);
    ASSERT(p.axiom_reduction_pct == 40);  /* (5-3)/5 = 40% */
    return 0;
}

int test_axiom_names(void) {
    /* A1: A ⊕ A = ⊥
     * A2: A → A = ⊤
     * A3: ⊥ → A = ⊤ */
    bool a1 = false, a2 = false, a3 = false;
    for (int i = 0; i < DIAGONAL_NUM_RULES; i++) {
        if (strstr(DIAGONAL_TABLE[i].axiom_name, "⊕") &&
            strstr(DIAGONAL_TABLE[i].axiom_name, "⊥")) a1 = true;
        if (strstr(DIAGONAL_TABLE[i].axiom_name, "→") &&
            strstr(DIAGONAL_TABLE[i].axiom_name, "⊤")) a2 = true;
        if (strstr(DIAGONAL_TABLE[i].axiom_name, "⊥ → A")) a3 = true;
    }
    ASSERT(a1); ASSERT(a2); ASSERT(a3);
    return 0;
}

int test_diagonal_main(void) {
    int failed = 0;
    failed += test_correspondence_complete();
    failed += test_table_consistency();
    failed += test_properties();
    failed += test_axiom_names();
    if (failed) {
        fprintf(stderr, "test_diagonal: %d test(s) FAILED\n", failed);
        return 1;
    }
    printf("test_diagonal: all tests PASSED\n");
    return 0;
}

#ifndef BINE_COMBINED_TESTS
int main(void) { return test_diagonal_main(); }
#endif
