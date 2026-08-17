/*
 * test_suite.c - Runs all BINE tests
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int test_bbf_main(void);
extern int test_diagonal_main(void);
extern int test_zentropy_main(void);
extern int test_kolmogorov_main(void);
extern int test_dfg_main(void);
extern int test_pure_binary_main(void);
extern int test_elf_pipeline_main(void);
extern int test_serialize_main(void);
extern int test_bbf_fuzz_main(void);
extern int test_compat_main(void);
extern int test_layout_main(void);
#if BINE_BUILD_GPU
extern int test_gpu_main(void);
#endif

int main(int argc, char **argv) {
    int total_failed = 0;
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "bbf",         test_bbf_main },
        { "diagonal",    test_diagonal_main },
        { "zentropy",    test_zentropy_main },
        { "kolmogorov",  test_kolmogorov_main },
        { "dfg",         test_dfg_main },
        { "pure_binary", test_pure_binary_main },
        { "elf_pipeline",test_elf_pipeline_main },
        { "serialize",   test_serialize_main },
        { "bbf_fuzz",    test_bbf_fuzz_main },
        { "compat",      test_compat_main },
        { "layout",      test_layout_main },
#if BINE_BUILD_GPU
        { "gpu",         test_gpu_main },
#endif
    };
    int n = sizeof(tests) / sizeof(tests[0]);
    /* Allow filtering: ./test_suite bbf diagonal */
    for (int i = 0; i < n; i++) {
        int run = (argc < 2);
        for (int a = 1; a < argc; a++) {
            if (strcmp(argv[a], tests[i].name) == 0) run = 1;
        }
        if (!run) continue;
        printf("\n=== Running test_%s ===\n", tests[i].name);
        int rc = tests[i].fn();
        if (rc != 0) total_failed++;
    }
    if (total_failed) {
        printf("\n*** %d test group(s) FAILED ***\n", total_failed);
        return 1;
    }
    printf("\n*** All test groups PASSED ***\n");
    return 0;
}
