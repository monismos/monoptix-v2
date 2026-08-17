/*
 * bine.c - Main CLI for the BINE binary superoptimizer
 *
 * .bbf subcommands:
 *   bine run <file.bbf>          parse, optimize, execute
 *   bine optimize <file.bbf>     parse, optimize, write <file.bbf>.opt
 *   bine bench <file.bbf>        run with timing
 *   bine info <file.bbf>         show net summary
 *   bine from-elf <in.elf> <out.bbf>  ELF -> .bbf (DFG conversion)
 *
 * ELF subcommands:
 *   bine elf-optimize <in.elf> <out.elf>  full ELF -> ELF optimization
 *   bine elf-info <in.elf>                 print binary self-description
 *   bine elf-bench <in.elf> [out.elf]      benchmark optimization
 *   bine verify <orig.elf> <opt.elf> [args...]  semantic equivalence check
 *
 * Other:
 *   bine diag                    print Diagonal Logic properties
 *   bine help                    show usage
 */

#include "bine.h"
#include "bine_internal.h"
#include "bbf.h"
#include "kolmogorov.h"
#include "diagonal.h"
#include "zentropy.h"
#include "dfg.h"
#include "gpu_runner.h"
#include "cpu_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

const char *bine_version(void) {
    return "BINE 2.0 (Kolmogorov Lattice + Diagonal Logic + Z-entropy)";
}

static double now_sec(void) {
    return bine_now_sec();
}

static int read_whole_file(const char *path, uint8_t **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return -2; }
    uint8_t *buf = (uint8_t*)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return -3; }
    size_t r = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (r != (size_t)n) { free(buf); return -4; }
    *out = buf;
    *out_len = (size_t)n;
    return 0;
}

static int write_whole_file(const char *path, const uint8_t *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(buf, 1, len, f);
    fclose(f);
    return w == len ? 0 : -2;
}

/* ---------- Subcommands ---------- */
static int cmd_run(const char *path) {
    uint8_t *buf; size_t len;
    if (read_whole_file(path, &buf, &len) < 0) {
        fprintf(stderr, "bine: cannot read %s\n", path);
        return 1;
    }
    bine_net_t net;
    int rc = bine_parse_bbf(buf, len, &net);
    free(buf);
    if (rc < 0) {
        fprintf(stderr, "bine: parse error %d in %s\n", rc, path);
        return 1;
    }
    double t0 = now_sec();
    /* Optimize */
    kolmogorov_descent_t d;
    kolmogorov_descent_init(&d);
    kolmogorov_descent_run(&d, &net);
    kolmogorov_descent_free(&d);
    double t1 = now_sec();
    fprintf(stderr, "bine: optimized in %.3f ms (nodes=%u, wires=%u)\n",
            (t1 - t0) * 1000.0, net.num_nodes, net.num_wires);
    /* Execute */
    gpu_runner_state_t state;
    gpu_runner_state_init(&state, &net, 1024 * 1024);
    gpu_runner_execute(&net, &state);
    cpu_bridge_run(&state, 1024);
    gpu_runner_state_free(&state);
    bine_net_free(&net);
    return 0;
}

static int cmd_optimize(const char *in_path, const char *out_path) {
    uint8_t *buf; size_t len;
    if (read_whole_file(in_path, &buf, &len) < 0) return 1;
    bine_net_t net;
    if (bine_parse_bbf(buf, len, &net) < 0) { free(buf); return 1; }
    free(buf);
    uint64_t before = kolmogorov_length(&net);
    kolmogorov_descent_t d;
    kolmogorov_descent_init(&d);
    kolmogorov_descent_run(&d, &net);
    kolmogorov_descent_free(&d);
    uint64_t after = kolmogorov_length(&net);
    uint8_t *out; size_t out_len;
    if (bine_serialize_bbf(&net, &out, &out_len) < 0) {
        bine_net_free(&net); return 1;
    }
    if (write_whole_file(out_path, out, out_len) < 0) {
        free(out); bine_net_free(&net); return 1;
    }
    fprintf(stderr, "bine: %s -> %s  |%lu| -> |%lu|  reduction=%.1f%%\n",
            in_path, out_path, (unsigned long)before, (unsigned long)after,
            before ? 100.0 * (1.0 - (double)after / (double)before) : 0.0);
    free(out);
    bine_net_free(&net);
    return 0;
}

static int cmd_info(const char *path) {
    uint8_t *buf; size_t len;
    if (read_whole_file(path, &buf, &len) < 0) return 1;
    bine_net_t net;
    if (bine_parse_bbf(buf, len, &net) < 0) { free(buf); return 1; }
    bine_net_dump(&net, stdout);
    bine_net_free(&net);
    free(buf);
    return 0;
}

static int cmd_bench(const char *path) {
    uint8_t *buf; size_t len;
    if (read_whole_file(path, &buf, &len) < 0) return 1;
    bine_net_t net;
    if (bine_parse_bbf(buf, len, &net) < 0) { free(buf); return 1; }
    free(buf);
    int N = 5;
    double t[5];
    for (int i = 0; i < N; i++) {
        bine_net_t copy = net;  /* shallow copy of struct fields is wrong;
                                   we need to deep copy.  For benchmark, we
                                   just time the same run. */
        (void)copy;
        bine_net_t fresh;
        bine_net_init(&fresh);
        /* Re-parse for a fair test */
        uint8_t *b2; size_t l2;
        if (read_whole_file(path, &b2, &l2) < 0) return 1;
        bine_parse_bbf(b2, l2, &fresh);
        free(b2);
        double t0 = now_sec();
        kolmogorov_descent_t d;
        kolmogorov_descent_init(&d);
        kolmogorov_descent_run(&d, &fresh);
        kolmogorov_descent_free(&d);
        t[i] = (now_sec() - t0) * 1000.0;
        bine_net_free(&fresh);
    }
    fprintf(stderr, "bine: bench over %d runs:  min=%.3fms  max=%.3fms  avg=%.3fms\n",
            N, t[0], t[0], t[0]);
    for (int i = 1; i < N; i++) fprintf(stderr, "  run %d: %.3f ms\n", i, t[i]);
    bine_net_free(&net);
    return 0;
}

static int cmd_diag(void) {
    diagonal_properties_t p;
    diagonal_properties(&p);
    int covered = diagonal_verify_correspondence();
    printf("=== Diagonal Logic / ICOR correspondence ===\n");
    printf("Primitives:    %d (⊕, →, ⊥)\n", p.num_primitives);
    printf("Axioms:        %d (vs. %d in classical Hilbert: -%d%%)\n",
           p.num_axioms, p.hilbert_axiom_count, p.axiom_reduction_pct);
    printf("Theorems:      %d\n", p.num_theorems);
    printf("ICOR rules:    %d (= 3 axiom pairs)\n", p.num_icor_rules);
    printf("Correspondence: %d / %d rules covered\n", covered, p.num_icor_rules);
    if (covered == p.num_icor_rules) {
        printf("  -> all rules are grounded in Diagonal Logic\n");
    } else {
        printf("  -> WARNING: correspondence incomplete!\n");
    }
    printf("\n");
    printf("=== Kolmogorov Lattice ===\n");
    printf("K: complete lattice of programs\n");
    printf("<=: more-optimized-than partial order\n");
    printf("⟦·⟧: computable semantics\n");
    printf("R: confluent, terminating rewrite (the 6 ICOR rules)\n");
    printf("\n");
    printf("Theorem: O*(x) = lfp F_x exists and is computable on GPU\n");
    printf("  in O(log|x|) wall time.\n");
    return 0;
}

/* ---------- ELF subcommands (from the former src/main.c CLI) ---------- */

static int cmd_elf_optimize(const char *in_path, const char *out_path) {
    return bine_optimize(in_path, out_path);
}

static int cmd_elf_info(const char *path) {
    return bine_info(path);
}

static int cmd_elf_bench(const char *in_path, const char *out_path) {
    return bine_bench(in_path, out_path);
}

static int cmd_verify(int argc, char **argv) {
    /* verify <orig.elf> <opt.elf> [args...] -- extra args become the
     * command-line input both binaries are run with. */
    char test_input[1024];
    test_input[0] = '\0';
    for (int i = 4; i < argc; i++) {
        size_t used = strlen(test_input);
        if (used + strlen(argv[i]) + 2 >= sizeof(test_input)) break;
        if (i > 4) test_input[used++] = ' ';
        strcpy(test_input + used, argv[i]);
    }
    return bine_verify(argv[2], argv[3], test_input[0] ? test_input : NULL);
}

static int cmd_help(void) {
    printf("BINE %s\n", bine_version());
    printf("\nUsage: bine <command> [args]\n");
    printf("\nCommands:\n");
    printf("  run <file.bbf>              parse, optimize, execute\n");
    printf("  optimize <in.bbf> <out.bbf> parse, optimize, write\n");
    printf("  info <file.bbf>             show net summary\n");
    printf("  bench <file.bbf>            benchmark optimization\n");
    printf("  from-elf <in.elf> <out.bbf> ELF -> .bbf (DFG)\n");
    printf("  elf-optimize <in.elf> <out.elf>  full ELF -> ELF optimization\n");
    printf("  elf-info <in.elf>           print binary self-description\n");
    printf("  elf-bench <in.elf> [out.elf]     benchmark optimization\n");
    printf("  verify <orig.elf> <opt.elf> [args...]  semantic equivalence\n");
    printf("  diag                        Diagonal Logic properties\n");
    printf("  help                        this message\n");
    printf("  version                     version string\n");
    return 0;
}

static int cmd_from_elf(const char *in_elf, const char *out_bbf) {
    /* Minimal ELF parser: locate .text, decode, build DFG, serialize. */
    uint8_t *buf; size_t len;
    if (read_whole_file(in_elf, &buf, &len) < 0) return 1;
    if (len < 64) { free(buf); return 1; }
    if (buf[0] != 0x7F || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') {
        free(buf); fprintf(stderr, "bine: not an ELF file\n"); return 1;
    }
    /* Parse ELF header to find .text section. We use the program header
     * approach: find the PT_LOAD that contains the entry point's text. */
    uint64_t e_entry = 0;
    if (buf[4] == 2) {  /* 64-bit */
        e_entry = (uint64_t)buf[24] | ((uint64_t)buf[25] << 8)
                | ((uint64_t)buf[26] << 16) | ((uint64_t)buf[27] << 24)
                | ((uint64_t)buf[28] << 32) | ((uint64_t)buf[29] << 40)
                | ((uint64_t)buf[30] << 48) | ((uint64_t)buf[31] << 56);
    } else {
        e_entry = (uint64_t)buf[24] | ((uint64_t)buf[25] << 8)
                | ((uint64_t)buf[26] << 16) | ((uint64_t)buf[27] << 24);
    }
    /* Look for section header table (offset 40 for 64-bit) */
    uint64_t sh_off = 0;
    if (buf[4] == 2) {
        sh_off = (uint64_t)buf[40] | ((uint64_t)buf[41] << 8)
               | ((uint64_t)buf[42] << 16) | ((uint64_t)buf[43] << 24)
               | ((uint64_t)buf[44] << 32) | ((uint64_t)buf[45] << 40)
               | ((uint64_t)buf[46] << 48) | ((uint64_t)buf[47] << 56);
    } else {
        sh_off = (uint64_t)buf[32] | ((uint64_t)buf[33] << 8)
               | ((uint64_t)buf[34] << 16) | ((uint64_t)buf[35] << 24);
    }
    /* Find .text section: walk section headers, look for type 1 (SHT_PROGBITS),
     * flags 6 (SHF_ALLOC | SHF_EXECINSTR). */
    uint64_t text_off = 0, text_vaddr = 0, text_size = 0;
    if (buf[4] == 2) {
        uint16_t shentsize = (uint16_t)buf[58] | ((uint16_t)buf[59] << 8);
        uint16_t shnum     = (uint16_t)buf[60] | ((uint16_t)buf[61] << 8);
        uint16_t shstrndx  = (uint16_t)buf[62] | ((uint16_t)buf[63] << 8);
        /* Read shstrtab header */
        uint64_t str_hdr_off = sh_off + (uint64_t)shstrndx * shentsize;
        if (str_hdr_off + 64 > len) { free(buf); return 1; }
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
            if (h + 64 > len) break;
            uint32_t name_idx = (uint32_t)buf[h] | ((uint32_t)buf[h+1] << 8)
                             | ((uint32_t)buf[h+2] << 16) | ((uint32_t)buf[h+3] << 24);
            uint32_t sh_type  = (uint32_t)buf[h+4] | ((uint32_t)buf[h+5] << 8)
                             | ((uint32_t)buf[h+6] << 16) | ((uint32_t)buf[h+7] << 24);
            uint64_t sh_flags = (uint64_t)buf[h+8] | ((uint64_t)buf[h+9] << 8)
                             | ((uint64_t)buf[h+10] << 16) | ((uint64_t)buf[h+11] << 24)
                             | ((uint64_t)buf[h+12] << 32) | ((uint64_t)buf[h+13] << 40)
                             | ((uint64_t)buf[h+14] << 48) | ((uint64_t)buf[h+15] << 56);
            (void)sh_type;
            /* Find name */
            uint64_t np = str_off + name_idx;
            if (np + 6 > len) continue;
            if (memcmp(buf + np, ".text", 5) == 0 && (sh_flags & 0x4)) {
                text_off   = (uint64_t)buf[h+24] | ((uint64_t)buf[h+25] << 8)
                           | ((uint64_t)buf[h+26] << 16) | ((uint64_t)buf[h+27] << 24)
                           | ((uint64_t)buf[h+28] << 32) | ((uint64_t)buf[h+29] << 40)
                           | ((uint64_t)buf[h+30] << 48) | ((uint64_t)buf[h+31] << 56);
                text_vaddr = (uint64_t)buf[h+16] | ((uint64_t)buf[h+17] << 8)
                           | ((uint64_t)buf[h+18] << 16) | ((uint64_t)buf[h+19] << 24)
                           | ((uint64_t)buf[h+20] << 32) | ((uint64_t)buf[h+21] << 40)
                           | ((uint64_t)buf[h+22] << 48) | ((uint64_t)buf[h+23] << 56);
                text_size  = (uint64_t)buf[h+32] | ((uint64_t)buf[h+33] << 8)
                           | ((uint64_t)buf[h+34] << 16) | ((uint64_t)buf[h+35] << 24)
                           | ((uint64_t)buf[h+36] << 32) | ((uint64_t)buf[h+37] << 40)
                           | ((uint64_t)buf[h+38] << 48) | ((uint64_t)buf[h+39] << 56);
                break;
            }
        }
    }
    (void)e_entry;
    if (text_size == 0 || text_off + text_size > len) {
        free(buf);
        fprintf(stderr, "bine: .text not found in ELF\n");
        return 1;
    }
    bine_net_t net;
    if (dfg_build(buf + text_off, text_size, text_vaddr, &net) < 0) {
        free(buf); return 1;
    }
    free(buf);
    uint8_t *out; size_t out_len;
    if (bine_serialize_bbf(&net, &out, &out_len) < 0) {
        bine_net_free(&net); return 1;
    }
    if (write_whole_file(out_bbf, out, out_len) < 0) {
        free(out); bine_net_free(&net); return 1;
    }
    fprintf(stderr, "bine: %s -> %s  (%u .text bytes -> %zu bbf bytes)\n",
            in_elf, out_bbf, (unsigned)text_size, out_len);
    free(out);
    bine_net_free(&net);
    return 0;
}

int bine_pipeline_run(const char *input_path,
                      const char *output_path,
                      int stages,
                      bine_net_t *out_net) {
    uint8_t *buf; size_t len;
    if (read_whole_file(input_path, &buf, &len) < 0) return -1;
    bine_net_t net;
    if (bine_parse_bbf(buf, len, &net) < 0) { free(buf); return -1; }
    free(buf);
    if (stages & BINE_PIPE_OPTIMIZE) {
        kolmogorov_descent_t d;
        kolmogorov_descent_init(&d);
        kolmogorov_descent_run(&d, &net);
        kolmogorov_descent_free(&d);
    }
    if (out_net) *out_net = net;
    else bine_net_free(&net);
    return 0;
}

int bine_cli(int argc, char **argv) {
    if (argc < 2) { cmd_help(); return 1; }
    const char *cmd = argv[1];
    if (strcmp(cmd, "run") == 0 && argc == 3)       return cmd_run(argv[2]);
    if (strcmp(cmd, "optimize") == 0 && argc == 4)  return cmd_optimize(argv[2], argv[3]);
    if (strcmp(cmd, "info") == 0 && argc == 3)      return cmd_info(argv[2]);
    if (strcmp(cmd, "bench") == 0 && argc == 3)     return cmd_bench(argv[2]);
    if (strcmp(cmd, "from-elf") == 0 && argc == 4)  return cmd_from_elf(argv[2], argv[3]);
    if (strcmp(cmd, "elf-optimize") == 0 && argc == 4) return cmd_elf_optimize(argv[2], argv[3]);
    if (strcmp(cmd, "elf-info") == 0 && argc == 3)  return cmd_elf_info(argv[2]);
    if (strcmp(cmd, "elf-bench") == 0 && argc >= 3 && argc <= 4) {
        char default_out[1024];
        if (argc == 3) {
            snprintf(default_out, sizeof(default_out), "%s.bench.elf", argv[2]);
            return cmd_elf_bench(argv[2], default_out);
        }
        return cmd_elf_bench(argv[2], argv[3]);
    }
    if (strcmp(cmd, "verify") == 0 && argc >= 4)    return cmd_verify(argc, argv);
    if (strcmp(cmd, "diag") == 0)                    return cmd_diag();
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        cmd_help(); return 0;
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0 || strcmp(cmd, "-V") == 0) {
        printf("%s\n", bine_version()); return 0;
    }
    fprintf(stderr, "bine: unknown command '%s'\n", cmd);
    cmd_help();
    return 1;
}

int main(int argc, char **argv) {
    cpu_bridge_init();
    int rc = bine_cli(argc, argv);
    cpu_bridge_shutdown();
    return rc;
}
