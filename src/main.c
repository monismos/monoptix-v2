#include "bine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char* prog) {
    printf("BINE v2 — Binary Interaction Net Executor (ELF superoptimizer)\n");
    printf("Usage: %s <command> [options]\n\n", prog);
    printf("Commands:\n");
    printf("  optimize <input.elf> <output.elf>    Full ELF binary optimization\n");
    printf("  info     <input.elf>                  Print binary self-description\n");
    printf("  verify   <orig.elf> <opt.elf>         Verify semantic equivalence\n");
    printf("  bench    <input.elf> [output.elf]     Benchmark optimization\n");
}

static bool is_help(const char* arg) {
    return !strcmp(arg, "--help") || !strcmp(arg, "-h") || !strcmp(arg, "help");
}

int main(int argc, char** argv) {
    if (argc < 2 || is_help(argv[1])) {
        print_usage(argv[0]);
        return 0;
    }
    const char* cmd = argv[1];
    if (!strcmp(cmd, "optimize")) {
        if (argc != 4) { fprintf(stderr, "Usage: %s optimize <input.elf> <output.elf>\n", argv[0]); return 1; }
        return bine_optimize(argv[2], argv[3]);
    } else if (!strcmp(cmd, "info")) {
        if (argc != 3) { fprintf(stderr, "Usage: %s info <input.elf>\n", argv[0]); return 1; }
        return bine_info(argv[2]);
    } else if (!strcmp(cmd, "verify")) {
        if (argc < 4) { fprintf(stderr, "Usage: %s verify <original.elf> <optimized.elf>\n", argv[0]); return 1; }
        return bine_verify(argv[2], argv[3], argc > 4 ? argv[4] : NULL);
    } else if (!strcmp(cmd, "bench")) {
        const char* out = argc > 3 ? argv[3] : "/tmp/bine_bench_out.elf";
        return bine_bench(argv[2], out);
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        print_usage(argv[0]);
        return 1;
    }
}
