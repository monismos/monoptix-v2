/*
 * test_compat.c - Tests for the portable compatibility layer (bine_compat)
 */

#include "bine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(name) static int test_##name(void)
#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

TEST(u128_from64_lo_hi) {
    bine_u128 a = bine_u128_from64(0x1122334455667788ULL);
    ASSERT(bine_u128_lo(a) == 0x1122334455667788ULL);
    ASSERT(bine_u128_hi(a) == 0);
    return 0;
}

TEST(u128_or_and_xor_not) {
    bine_u128 a = bine_u128_from64(0xF0F0ULL);
    bine_u128 b = bine_u128_from64(0x0F0FULL);
    ASSERT(bine_u128_lo(bine_u128_or(a, b)) == 0xFFFFULL);
    ASSERT(bine_u128_lo(bine_u128_and(a, b)) == 0);
    ASSERT(bine_u128_lo(bine_u128_xor(a, b)) == 0xFFFFULL);
    ASSERT(bine_u128_lo(bine_u128_not(bine_u128_from64(0))) == ~0ULL);
    ASSERT(bine_u128_hi(bine_u128_not(bine_u128_from64(0))) == ~0ULL);
    return 0;
}

TEST(u128_iszero_shl_popcount) {
    ASSERT(bine_u128_iszero(bine_u128_from64(0)));
    ASSERT(!bine_u128_iszero(bine_u128_from64(1)));
    bine_u128 s = bine_u128_shl(bine_u128_from64(1), 70);
    ASSERT(bine_u128_lo(s) == 0 && bine_u128_hi(s) == (1ULL << 6));
    bine_u128 p = bine_u128_from64(0xFF);
    ASSERT(bine_u128_popcount(p) == 8);
    return 0;
}

TEST(strcasecmp) {
    ASSERT(bine_strcasecmp("Hello", "HELLO") == 0);
    ASSERT(bine_strcasecmp("abc", "abd") < 0);
    ASSERT(bine_strcasecmp("z", "a") > 0);
    return 0;
}

TEST(file_roundtrip) {
    const char *dir = bine_state_dir();
    ASSERT(dir && dir[0] != 0);
    char path[1024];
    snprintf(path, sizeof(path), "%s/bine_test_compat.bin", dir);

    uint8_t data[64];
    for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i * 7 + 3);
    ASSERT(bine_write_file(path, data, sizeof(data)) == 0);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT(bine_read_file(path, &out, &out_len) == 0);
    ASSERT(out_len == 64);
    ASSERT(memcmp(out, data, 64) == 0);
    free(out);

    /* Copy + byte-exactness */
    char path2[1024];
    snprintf(path2, sizeof(path2), "%s/bine_test_compat2.bin", dir);
    ASSERT(bine_file_copy(path, path2) == 0);
    ASSERT(bine_read_file(path2, &out, &out_len) == 0);
    ASSERT(out_len == 64 && memcmp(out, data, 64) == 0);
    free(out);

    bine_chmod_exec(path2); /* must not crash */
    return 0;
}

TEST(time_monotonic) {
    uint64_t t0 = bine_time_ms();
    uint64_t t1 = bine_time_ms();
    ASSERT(t1 >= t0);
    double s0 = bine_now_sec();
    double s1 = bine_now_sec();
    ASSERT(s1 >= s0 && s0 > 0.0);
    return 0;
}

TEST(run_capture) {
    char *out = NULL;
    size_t len = 0;
    int status = -1;
    /* `echo hi` works through cmd.exe on Windows and /bin/sh on POSIX. */
    ASSERT(bine_run_capture("echo hi", &out, &len, &status) == 0);
    ASSERT(status == 0);
    ASSERT(out && len > 0);
    free(out);
    return 0;
}

int test_compat_main(void) {
    int failed = 0;
    failed += test_u128_from64_lo_hi();
    failed += test_u128_or_and_xor_not();
    failed += test_u128_iszero_shl_popcount();
    failed += test_strcasecmp();
    failed += test_file_roundtrip();
    failed += test_time_monotonic();
    failed += test_run_capture();
    if (failed) {
        fprintf(stderr, "test_compat: %d test(s) FAILED\n", failed);
        return 1;
    }
    printf("test_compat: all tests PASSED\n");
    return 0;
}

#ifndef BINE_COMBINED_TESTS
int main(void) { return test_compat_main(); }
#endif
