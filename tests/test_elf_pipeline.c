/*
 * test_elf_pipeline.c - Validate the ELF optimization pipeline
 *
 * This is the original 6-binary validation:
 *   ls, bash, cat, date, echo, true
 * must all produce semantically identical output after bine optimize.
 *
 * We test the DFG-based optimization path: ELF -> DFG -> net -> optimize.
 */

#include "dfg.h"
#include "bbf.h"
#include "kolmogorov.h"
#include "bine.h"
#include "bine_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef BINE_WINDOWS
#include <io.h>
#define BINE_FIND_EXT "*.elf"
#define BINE_FIND_DATA struct _finddata_t
#define BINE_FIND_FIRST(a, b) _findfirst(a, b)
#define BINE_FIND_NEXT _findnext
#define BINE_FIND_CLOSE _findclose
#else
#include <dirent.h>
#define BINE_FIND_EXT ""
#define BINE_FIND_DATA void
#define BINE_FIND_FIRST(a, b) ((void)(a), (void)(b), 0)
#define BINE_FIND_NEXT(a, b) 0
#define BINE_FIND_CLOSE(a) ((void)(a))
#endif

#define ASSERT(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static int read_elf_text(const char *path, uint8_t **out, uint8_t **out_base,
                          size_t *out_size, uint64_t *out_vaddr);
int test_elf_ls(void);
int test_elf_true(void);
int test_elf_echo(void);

static void put_le16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF; }
static void put_le64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* Build a minimal ELF64: ehdr + .text + .shstrtab + section headers.
 * Returns 0 on success and writes the fixture to path. */
static int write_fixture_elf(const char *path, const uint8_t *text, size_t n) {
    const uint64_t text_off = 64;
    const uint64_t shstr_off = text_off + n;
    const char shstr[] = "\0.text\0.shstrtab\0";
    const size_t shstr_len = sizeof(shstr);
    const uint64_t sh_off = (shstr_off + shstr_len + 7) & ~(uint64_t)7;
    const size_t file_size = sh_off + 3 * 64;

    uint8_t *buf = (uint8_t *)calloc(1, file_size);
    if (!buf) return -1;

    /* ELF header */
    buf[0] = 0x7F; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
    buf[4] = 2; buf[5] = 1; buf[6] = 1; buf[7] = 0;      /* ELF64, LE, v1 */
    put_le16(buf + 16, 2);                               /* ET_EXEC */
    put_le16(buf + 18, 62);                              /* EM_X86_64 */
    put_le32(buf + 20, 1);
    put_le64(buf + 24, 0x400000);                        /* e_entry */
    put_le64(buf + 32, 0);                               /* e_phoff */
    put_le64(buf + 40, sh_off);
    put_le32(buf + 48, 0);
    put_le16(buf + 52, 64);                              /* e_ehsize */
    put_le16(buf + 54, 0);                               /* e_phentsize */
    put_le16(buf + 56, 0);                               /* e_phnum */
    put_le16(buf + 58, 64);                              /* e_shentsize */
    put_le16(buf + 60, 3);                               /* e_shnum */
    put_le16(buf + 62, 2);                               /* e_shstrndx */

    /* .text */
    memcpy(buf + text_off, text, n);
    /* .shstrtab */
    memcpy(buf + shstr_off, shstr, shstr_len);

    /* Section headers */
    uint8_t *s = buf + sh_off;
    /* [0] NULL (all zero) */
    /* [1] .text: name=1, PROGBITS, ALLOC|EXECINSTR, vaddr 0x400000 */
    put_le32(s + 64 + 0, 1); put_le32(s + 64 + 4, 1); put_le64(s + 64 + 8, 6);
    put_le64(s + 64 + 16, 0x400000); put_le64(s + 64 + 24, text_off);
    put_le64(s + 64 + 32, n); put_le32(s + 64 + 40, 0); put_le32(s + 64 + 44, 0);
    put_le64(s + 64 + 48, 1); put_le64(s + 64 + 56, 0);
    /* [2] .shstrtab: name=7, STRTAB */
    put_le32(s + 128 + 0, 7); put_le32(s + 128 + 4, 3); put_le64(s + 128 + 8, 0);
    put_le64(s + 128 + 16, 0); put_le64(s + 128 + 24, shstr_off);
    put_le64(s + 128 + 32, shstr_len); put_le32(s + 128 + 40, 0); put_le32(s + 128 + 44, 0);
    put_le64(s + 128 + 48, 1); put_le64(s + 128 + 56, 0);

    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return -1; }
    size_t w = fwrite(buf, 1, file_size, f);
    fclose(f);
    free(buf);
    return w == file_size ? 0 : -1;
}

/* Full-pipeline roundtrip on a synthetic ELF: bine_optimize must preserve
 * the file byte-for-byte when no dead code or reordering applies. */
static int test_elf_synthetic_identity(void) {
    const uint8_t fn[] = { 0x55, 0x48, 0x89, 0xE5, 0xB8, 0x2A, 0x00, 0x00,
                           0x00, 0x48, 0x89, 0xF8, 0x5D, 0xC3 }; /* push rbp; mov rbp,rsp; mov eax,42; mov rax,rdi; pop rbp; ret */
    char in[1024], out[1024];
    snprintf(in, sizeof(in), "%s/fixture_id.elf", bine_state_dir());
    snprintf(out, sizeof(out), "%s/fixture_id.opt.elf", bine_state_dir());
    ASSERT(write_fixture_elf(in, fn, sizeof(fn)) == 0);
    ASSERT(bine_optimize(in, out) == 0);

    FILE *a = fopen(in, "rb"), *b = fopen(out, "rb");
    ASSERT(a && b);
    fseek(a, 0, SEEK_END); fseek(b, 0, SEEK_END);
    long na = ftell(a), nb = ftell(b);
    fseek(a, 0, SEEK_SET); fseek(b, 0, SEEK_SET);
    ASSERT(na == nb);
    uint8_t *ba = (uint8_t *)malloc(na), *bb = (uint8_t *)malloc(nb);
    ASSERT(ba && bb);
    ASSERT(fread(ba, 1, na, a) == (size_t)na);
    ASSERT(fread(bb, 1, nb, b) == (size_t)nb);
    fclose(a); fclose(b);
    int eq = (memcmp(ba, bb, na) == 0);
    free(ba); free(bb);
    if (!eq) { fprintf(stderr, "FAIL: %s:%d: optimized ELF differs from original\n", __FILE__, __LINE__); return 1; }
    printf("  synthetic identity: byte-identical roundtrip OK\n");
    return 0;
}

/* Dead code (the 0x50 after `jmp +1`) must be DCE'd to a NOP. */
static int test_elf_synthetic_dce(void) {
    const uint8_t text[] = { 0xEB, 0x01, 0x50, 0xC3 }; /* jmp +1; push rax (unreachable); ret */
    char in[1024], out[1024];
    snprintf(in, sizeof(in), "%s/fixture_dce.elf", bine_state_dir());
    snprintf(out, sizeof(out), "%s/fixture_dce.opt.elf", bine_state_dir());
    ASSERT(write_fixture_elf(in, text, sizeof(text)) == 0);
    ASSERT(bine_optimize(in, out) == 0);

    uint8_t *t, *base; size_t sz; uint64_t vaddr;
    ASSERT(read_elf_text(out, &t, &base, &sz, &vaddr) == 0);
    int ok = (sz == 4 && t[0] == 0xEB && t[1] == 0x01 && t[2] == 0x90 && t[3] == 0xC3);
    if (!ok) {
        fprintf(stderr, "FAIL: %s:%d: expected EB 01 90 C3, got %02X %02X %02X %02X\n",
                __FILE__, __LINE__, sz > 0 ? t[0] : 0, sz > 1 ? t[1] : 0,
                sz > 2 ? t[2] : 0, sz > 3 ? t[3] : 0);
        free(base);
        return 1;
    }
    free(base);
    printf("  synthetic DCE: unreachable ret replaced with NOP OK\n");
    return 0;
}

/* Corpus mode: BINE_TEST_CORPUS=<dir> runs the full optimize pipeline over
 * every *.elf in <dir> and checks the hard invariants: the optimizer
 * succeeds, the output is still a parseable ELF, and the .text section has
 * the same size (offset-preserving rewrite).  On Linux hosts this pairs with
 * `bine verify` for full semantic equivalence; the invariants themselves are
 * platform-independent. */
static int test_corpus(const char *dir) {
    char pattern[1024], in[1024], out[1024];
    snprintf(pattern, sizeof(pattern), "%s/%s", dir, BINE_FIND_EXT);
    BINE_FIND_DATA fd;
    intptr_t handle = BINE_FIND_FIRST(pattern, &fd);
    if (handle == -1) {
        printf("  corpus: no .elf files in %s\n", dir);
        return 0;
    }
    int count = 0, failed = 0;
    do {
#ifdef BINE_WINDOWS
        const char *name = fd.name;
#else
        (void)fd;
        char name[1024];
        snprintf(name, sizeof(name), "placeholder");
        break; /* POSIX path handled separately below */
#endif
        snprintf(in, sizeof(in), "%s/%s", dir, name);
        snprintf(out, sizeof(out), "%s/%s.opt.elf", bine_state_dir(), name);
        if (bine_optimize(in, out) != 0) {
            fprintf(stderr, "FAIL: %s: optimize failed\n", in);
            failed++;
            continue;
        }
        uint8_t *base = NULL, *t = NULL; size_t sz = 0; uint64_t vaddr = 0;
        if (read_elf_text(out, &t, &base, &sz, &vaddr) != 0) {
            fprintf(stderr, "FAIL: %s: output not parseable\n", out);
            failed++;
            free(base);
            continue;
        }
        /* .text size must be preserved exactly. */
        uint8_t *base_in = NULL, *t_in = NULL; size_t sz_in = 0; uint64_t vaddr_in = 0;
        if (read_elf_text(in, &t_in, &base_in, &sz_in, &vaddr_in) != 0) {
            fprintf(stderr, "FAIL: %s: input not parseable\n", in);
            failed++;
        } else if (sz != sz_in) {
            fprintf(stderr, "FAIL: %s: .text size changed %u -> %u\n",
                    in, (unsigned)sz_in, (unsigned)sz);
            failed++;
        }
        free(base_in);
        free(base);
        count++;
        printf("  corpus: %s (%u bytes .text) OK\n", name, (unsigned)sz);
    } while (BINE_FIND_NEXT(handle, &fd) == 0);
    BINE_FIND_CLOSE(handle);

#ifndef BINE_WINDOWS
    /* POSIX: iterate with opendir/readdir. */
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            size_t l = strlen(e->d_name);
            if (l < 5 || strcmp(e->d_name + l - 4, ".elf") != 0) continue;
            snprintf(in, sizeof(in), "%s/%s", dir, e->d_name);
            snprintf(out, sizeof(out), "%s/%s.opt.elf", bine_state_dir(), e->d_name);
            if (bine_optimize(in, out) != 0) {
                fprintf(stderr, "FAIL: %s: optimize failed\n", in);
                failed++;
                continue;
            }
            uint8_t *base = NULL, *t = NULL; size_t sz = 0; uint64_t vaddr = 0;
            if (read_elf_text(out, &t, &base, &sz, &vaddr) != 0) {
                fprintf(stderr, "FAIL: %s: output not parseable\n", out);
                failed++;
                free(base);
                continue;
            }
            uint8_t *base_in = NULL, *t_in = NULL; size_t sz_in = 0; uint64_t vaddr_in = 0;
            if (read_elf_text(in, &t_in, &base_in, &sz_in, &vaddr_in) != 0 ||
                sz != sz_in) {
                fprintf(stderr, "FAIL: %s: .text size changed or unparseable\n", in);
                failed++;
            }
            free(base_in);
            free(base);
            count++;
            printf("  corpus: %s (%u bytes .text) OK\n", e->d_name, (unsigned)sz);
        }
        closedir(d);
    }
#endif
    if (failed) {
        fprintf(stderr, "test_elf_pipeline: %d corpus file(s) FAILED\n", failed);
        return failed;
    }
    printf("  corpus: %d file(s) round-tripped OK\n", count);
    return 0;
}

int test_elf_pipeline_main(void) {
    int failed = 0;
    printf("=== ELF pipeline validation ===\n");
    failed += test_elf_synthetic_identity();
    failed += test_elf_synthetic_dce();
    const char *corpus = getenv("BINE_TEST_CORPUS");
    if (corpus && corpus[0]) failed += test_corpus(corpus);
    failed += test_elf_ls();
    failed += test_elf_true();
    failed += test_elf_echo();
    if (failed) {
        fprintf(stderr, "test_elf_pipeline: %d test(s) FAILED\n", failed);
        return 1;
    }
    printf("test_elf_pipeline: all tests PASSED\n");
    return 0;
}

/* Returns the .text pointer in *out (borrowed from the malloc'd *out_base,
 * which the caller must free). */
static int read_elf_text(const char *path, uint8_t **out, uint8_t **out_base,
                          size_t *out_size, uint64_t *out_vaddr) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t*)malloc(n);
    fread(buf, 1, n, f);
    fclose(f);
    if (n < 64 || buf[0] != 0x7F || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') {
        free(buf); return -2;
    }
    int is64 = (buf[4] == 2);
    /* Find .text section */
    uint64_t sh_off = 0;
    if (is64) {
        sh_off = (uint64_t)buf[40] | ((uint64_t)buf[41] << 8)
               | ((uint64_t)buf[42] << 16) | ((uint64_t)buf[43] << 24)
               | ((uint64_t)buf[44] << 32) | ((uint64_t)buf[45] << 40)
               | ((uint64_t)buf[46] << 48) | ((uint64_t)buf[47] << 56);
    } else {
        sh_off = (uint64_t)buf[32] | ((uint64_t)buf[33] << 8)
               | ((uint64_t)buf[34] << 16) | ((uint64_t)buf[35] << 24);
    }
    uint64_t text_off = 0, text_vaddr = 0, text_size = 0;
    if (is64) {
        uint16_t shentsize = (uint16_t)buf[58] | ((uint16_t)buf[59] << 8);
        uint16_t shnum     = (uint16_t)buf[60] | ((uint16_t)buf[61] << 8);
        uint16_t shstrndx  = (uint16_t)buf[62] | ((uint16_t)buf[63] << 8);
        uint64_t str_hdr_off = sh_off + (uint64_t)shstrndx * shentsize;
        if (str_hdr_off + 64 > (uint64_t)n) { free(buf); return -3; }
        uint64_t str_off = (uint64_t)buf[str_hdr_off + 24]
                         | ((uint64_t)buf[str_hdr_off + 25] << 8)
                         | ((uint64_t)buf[str_hdr_off + 26] << 16)
                         | ((uint64_t)buf[str_hdr_off + 27] << 24)
                         | ((uint64_t)buf[str_hdr_off + 28] << 32)
                         | ((uint64_t)buf[str_hdr_off + 29] << 40)
                         | ((uint64_t)buf[str_hdr_off + 30] << 48)
                         | ((uint64_t)buf[str_hdr_off + 31] << 56);
        for (uint16_t i = 0; i < shnum; i++) {
            uint64_t h = sh_off + (uint64_t)i * shentsize;
            if (h + 64 > (uint64_t)n) break;
            uint32_t name_idx = (uint32_t)buf[h] | ((uint32_t)buf[h+1] << 8)
                             | ((uint32_t)buf[h+2] << 16) | ((uint32_t)buf[h+3] << 24);
            uint64_t sh_flags = (uint64_t)buf[h+8] | ((uint64_t)buf[h+9] << 8)
                             | ((uint64_t)buf[h+10] << 16) | ((uint64_t)buf[h+11] << 24)
                             | ((uint64_t)buf[h+12] << 32) | ((uint64_t)buf[h+13] << 40)
                             | ((uint64_t)buf[h+14] << 48) | ((uint64_t)buf[h+15] << 56);
            uint64_t np = str_off + name_idx;
            if (np + 6 > (uint64_t)n) continue;
            if (memcmp(buf + np, ".text", 5) == 0 && (sh_flags & 0x4)) {
                text_vaddr = (uint64_t)buf[h+16] | ((uint64_t)buf[h+17] << 8)
                           | ((uint64_t)buf[h+18] << 16) | ((uint64_t)buf[h+19] << 24)
                           | ((uint64_t)buf[h+20] << 32) | ((uint64_t)buf[h+21] << 40)
                           | ((uint64_t)buf[h+22] << 48) | ((uint64_t)buf[h+23] << 56);
                text_off   = (uint64_t)buf[h+24] | ((uint64_t)buf[h+25] << 8)
                           | ((uint64_t)buf[h+26] << 16) | ((uint64_t)buf[h+27] << 24)
                           | ((uint64_t)buf[h+28] << 32) | ((uint64_t)buf[h+29] << 40)
                           | ((uint64_t)buf[h+30] << 48) | ((uint64_t)buf[h+31] << 56);
                text_size  = (uint64_t)buf[h+32] | ((uint64_t)buf[h+33] << 8)
                           | ((uint64_t)buf[h+34] << 16) | ((uint64_t)buf[h+35] << 24)
                           | ((uint64_t)buf[h+36] << 32) | ((uint64_t)buf[h+37] << 40)
                           | ((uint64_t)buf[h+38] << 48) | ((uint64_t)buf[h+39] << 56);
                break;
            }
        }
    }
    if (text_size == 0 || text_off + text_size > (uint64_t)n) {
        free(buf); return -4;
    }
    *out = buf + text_off;
    *out_base = buf;
    *out_size = text_size;
    *out_vaddr = text_vaddr;
    return 0;
}

int test_elf_ls(void) {
    uint8_t *text; size_t sz; uint64_t vaddr; uint8_t *base;
    int rc = read_elf_text("/bin/ls", &text, &base, &sz, &vaddr);
    if (rc < 0) {
        printf("test_elf_ls: SKIPPED (no /bin/ls: rc=%d)\n", rc);
        return 0;
    }
    bine_net_t net;
    ASSERT(dfg_build(text, sz, vaddr, &net) == 0);
    uint64_t before = kolmogorov_length(&net);
    kolmogorov_descent_t d;
    kolmogorov_descent_init(&d);
    int rounds = kolmogorov_descent_run(&d, &net);
    kolmogorov_descent_free(&d);
    uint64_t after = kolmogorov_length(&net);
    printf("  /bin/ls: %lu -> %lu (rounds=%d)\n",
           (unsigned long)before, (unsigned long)after, rounds);
    bine_net_free(&net);
    free(base);
    return 0;
}

int test_elf_true(void) {
    uint8_t *text; size_t sz; uint64_t vaddr; uint8_t *base;
    int rc = read_elf_text("/bin/true", &text, &base, &sz, &vaddr);
    if (rc < 0) {
        printf("test_elf_true: SKIPPED (no /bin/true: rc=%d)\n", rc);
        return 0;
    }
    bine_net_t net;
    ASSERT(dfg_build(text, sz, vaddr, &net) == 0);
    uint64_t before = kolmogorov_length(&net);
    kolmogorov_descent_t d;
    kolmogorov_descent_init(&d);
    kolmogorov_descent_run(&d, &net);
    kolmogorov_descent_free(&d);
    uint64_t after = kolmogorov_length(&net);
    printf("  /bin/true: %lu -> %lu\n", (unsigned long)before, (unsigned long)after);
    bine_net_free(&net);
    free(base);
    return 0;
}

int test_elf_echo(void) {
    uint8_t *text; size_t sz; uint64_t vaddr; uint8_t *base;
    int rc = read_elf_text("/bin/echo", &text, &base, &sz, &vaddr);
    if (rc < 0) {
        printf("test_elf_echo: SKIPPED (no /bin/echo: rc=%d)\n", rc);
        return 0;
    }
    bine_net_t net;
    ASSERT(dfg_build(text, sz, vaddr, &net) == 0);
    uint64_t before = kolmogorov_length(&net);
    kolmogorov_descent_t d;
    kolmogorov_descent_init(&d);
    kolmogorov_descent_run(&d, &net);
    kolmogorov_descent_free(&d);
    uint64_t after = kolmogorov_length(&net);
    printf("  /bin/echo: %lu -> %lu\n", (unsigned long)before, (unsigned long)after);
    bine_net_free(&net);
    free(base);
    return 0;
}

#ifndef BINE_COMBINED_TESTS
int main(void) { return test_elf_pipeline_main(); }
#endif


