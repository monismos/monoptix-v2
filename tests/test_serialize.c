/*
 * test_serialize.c - End-to-end serializer test: synthetic binary -> net ->
 * bine_serialize_elf -> verify patched .text in the output file.
 */

#include "bine.h"
#include "icor/bits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(name) static int test_##name(void)
#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

/* xor eax,eax / xor eax,eax / ret  — middle insn made dead */
static const uint8_t image[] = { 0x31, 0xC0, 0x31, 0xC0, 0xC3 };

static BinaryIntrinsic *make_intrinsic(const char *path) {
    BinaryIntrinsic *bin = (BinaryIntrinsic *)calloc(1, sizeof(BinaryIntrinsic));
    if (!bin) return NULL;
    bin->e_machine = EM_X86_64;
    bin->e_entry = 0;
    bin->text_bytes = (uint8_t *)malloc(sizeof(image));
    if (!bin->text_bytes) { free(bin); return NULL; }
    memcpy(bin->text_bytes, image, sizeof(image));
    bin->text_size = sizeof(image);
    bin->text_vaddr = 0;
    bin->load_vaddr = 0;
    bin->load_offset = 0;
    bin->path = strdup(path);
    return bin;
}

static int write_image(const char *path) {
    return bine_write_file(path, image, sizeof(image)) == 0 ? 0 : -1;
}

TEST(serialize_nops_dead_insn) {
    const char *dir = bine_state_dir();
    ASSERT(dir);
    char src[1024], dst[1024];
    snprintf(src, sizeof(src), "%s/bine_ser_src.bin", dir);
    snprintf(dst, sizeof(dst), "%s/bine_ser_dst.bin", dir);
    ASSERT(write_image(src) == 0);

    BinaryIntrinsic *bin = make_intrinsic(src);
    ASSERT(bin);

    uint64_t net[3];
    net[0] = SET_TYPE(0, AGENT_CON);
    net[1] = SET_TYPE(0, AGENT_ERA);   /* dead */
    net[2] = SET_ESSENTIAL(SET_TYPE(0, AGENT_CON)); /* ret */

    ASSERT(bine_serialize_elf(net, 3, bin, dst) == 0);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT(bine_read_file(dst, &out, &out_len) == 0);
    ASSERT(out_len == sizeof(image)); /* size preserved */
    const uint8_t expect[] = { 0x31, 0xC0, 0x66, 0x90, 0xC3 };
    ASSERT(memcmp(out, expect, sizeof(expect)) == 0);

    free(out);
    bine_free_intrinsic(bin);
    return 0;
}

TEST(serialize_identity_when_no_dead) {
    const char *dir = bine_state_dir();
    ASSERT(dir);
    char src[1024], dst[1024];
    snprintf(src, sizeof(src), "%s/bine_ser_src2.bin", dir);
    snprintf(dst, sizeof(dst), "%s/bine_ser_dst2.bin", dir);
    ASSERT(write_image(src) == 0);

    BinaryIntrinsic *bin = make_intrinsic(src);
    ASSERT(bin);

    uint64_t net[3];
    net[0] = SET_TYPE(0, AGENT_CON);
    net[1] = SET_TYPE(0, AGENT_CON);
    net[2] = SET_ESSENTIAL(SET_TYPE(0, AGENT_CON));

    ASSERT(bine_serialize_elf(net, 3, bin, dst) == 0);

    uint8_t *out = NULL;
    size_t out_len = 0;
    ASSERT(bine_read_file(dst, &out, &out_len) == 0);
    ASSERT(out_len == sizeof(image));
    ASSERT(memcmp(out, image, sizeof(image)) == 0);

    free(out);
    bine_free_intrinsic(bin);
    return 0;
}

int test_serialize_main(void) {
    int failed = 0;
    failed += test_serialize_nops_dead_insn();
    failed += test_serialize_identity_when_no_dead();
    if (failed) {
        fprintf(stderr, "test_serialize: %d test(s) FAILED\n", failed);
        return 1;
    }
    printf("test_serialize: all tests PASSED\n");
    return 0;
}

#ifndef BINE_COMBINED_TESTS
int main(void) { return test_serialize_main(); }
#endif
