// api/api.c — BINE API implementation
// Main API functions: bine_optimize, bine_verify, bine_info, bine_bench
//
// Pipeline (all stages slot-stable: agent i ↔ instruction i, offsets and
// .text size never change):
//   extract → masks → peephole → build_net → ICOR reduce (CPU, slot-stable)
//   → STRAND-P schedule order → offset-preserving serialize.

#include "bine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// External GPU wrapper functions (linked from bine_gpu when CUDA is
// available; the CPU build provides bine_icor_reduce_cpu/bine_strand_p.c).
extern int bine_gpu_icor_reduce_wrapper(const uint64_t* net, uint32_t n_agents,
                                         uint64_t** reduced_net, uint32_t* n_reduced,
                                         uint32_t* rounds);
extern int bine_strand_p_optimize_wrapper(const uint64_t* net, uint32_t n_agents,
                                           const uint8_t* masks_r, const uint8_t* masks_w,
                                           const uint8_t* masks_k,
                                           uint64_t** scheduled_net, uint32_t* n_scheduled);

// Slot-stable reduction.  Always uses the host reducer: the ELF net's port
// fields hold DFG register numbers (not agent references), so the GPU
// compacting reducer would renumber agents and corrupt instruction mapping.
uint64_t* bine_icor_reduce(const uint64_t* net, uint32_t n_agents,
                          uint32_t* n_reduced, uint32_t* rounds) {
    uint64_t* reduced_net = NULL;
    uint32_t n_out = 0, r = 0;
    if (bine_icor_reduce_cpu(net, n_agents, &reduced_net, &n_out, &r) != 0) {
        fprintf(stderr, "[BINE-ICOR] slot-stable reduction failed\n");
        return NULL;
    }
    if (n_reduced) *n_reduced = n_out;
    if (rounds) *rounds = r;
    return reduced_net;
}

// STRAND-P optimization: produce a schedule order (permutation) that the
// serializer realizes with provably-safe same-length swaps.  Returns the
// order as a malloc'd uint32_t array via *out_order; the net is NOT
// reordered.
int bine_strand_p_optimize(const uint64_t* net, uint32_t n_agents,
                           const StrandMasks* masks,
                           uint32_t** out_order, uint32_t* n_scheduled) {
    if (!net || !masks || !out_order || !n_scheduled) return -1;

    uint8_t* masks_r = (uint8_t*)malloc(n_agents * sizeof(uint8_t));
    uint8_t* masks_w = (uint8_t*)malloc(n_agents * sizeof(uint8_t));
    uint8_t* masks_k = (uint8_t*)malloc(n_agents * sizeof(uint8_t));
    if (!masks_r || !masks_w || !masks_k) {
        free(masks_r);
        free(masks_w);
        free(masks_k);
        return -1;
    }
    for (uint32_t i = 0; i < n_agents; i++) {
        masks_r[i] = (uint8_t)bine_u128_lo(masks->M_R);
        masks_w[i] = (uint8_t)bine_u128_lo(masks->M_W);
        masks_k[i] = (uint8_t)bine_u128_lo(masks->M_K);
    }

    uint32_t* order = (uint32_t*)malloc(n_agents * sizeof(uint32_t));
    if (!order) {
        free(masks_r); free(masks_w); free(masks_k);
        return -1;
    }
    if (bine_strand_p_schedule_cpu(n_agents, masks_r, masks_w, masks_k, order) != 0) {
        free(masks_r); free(masks_w); free(masks_k);
        free(order);
        return -1;
    }

    free(masks_r);
    free(masks_w);
    free(masks_k);

    *out_order = order;
    *n_scheduled = n_agents;
    return 0;
}

// Main optimization function
int bine_optimize(const char* input_path, const char* output_path) {
    uint64_t start_time = bine_time_ms();

    // Stage 1: Extract intrinsic structure
    BinaryIntrinsic* bin = bine_extract_intrinsic(input_path);
    if (!bin) {
        return -1;
    }
    uint64_t stage1_time = bine_time_ms();

    // Stage 2: Derive STRAND masks
    StrandMasks masks = bine_derive_masks(bin->text_bytes, bin->text_size);
    uint64_t stage2_time = bine_time_ms();

    // Stage 2.5: Peephole optimization on raw .text bytes (size-preserving).
    {
        uint64_t peep_new_size = 0;
        int peep_rc = bine_peephole_optimize(bin->text_bytes, bin->text_size, &peep_new_size);
        if (peep_rc == 0) {
            fprintf(stderr, "[BINE-PEEPHOLE] Peephole pass complete on %lu byte .text\n",
                    (unsigned long)bin->text_size);
        }
    }
    uint64_t stage2b_time = bine_time_ms();

    // Stage 3: Build interaction net
    uint32_t n_agents = 0;
    uint32_t root_wire = 0;
    uint64_t* net = bine_build_net(bin, &masks, &n_agents, &root_wire);
    if (!net) {
        bine_free_intrinsic(bin);
        return -1;
    }
    uint64_t stage3_time = bine_time_ms();

    // Stage 4: Slot-stable ICOR reduction
    uint32_t n_reduced = 0;
    uint32_t icor_rounds = 0;
    uint64_t* reduced_net = bine_icor_reduce(net, n_agents, &n_reduced, &icor_rounds);
    if (!reduced_net) {
        bine_free_net(net);
        bine_free_intrinsic(bin);
        return -1;
    }
    uint64_t stage4_time = bine_time_ms();

    // Stage 5: STRAND-P schedule order
    uint32_t n_scheduled = 0;
    uint32_t* schedule = NULL;
    if (bine_strand_p_optimize(reduced_net, n_reduced, &masks, &schedule, &n_scheduled) != 0) {
        free(reduced_net);
        bine_free_net(net);
        bine_free_intrinsic(bin);
        return -1;
    }
    uint64_t stage5_time = bine_time_ms();

    // Stage 6: Serialize to ELF (offset-preserving: DCE + safe reorder)
    if (bine_serialize_elf_ex(reduced_net, n_reduced, schedule, n_scheduled,
                              bin, output_path) < 0) {
        free(schedule);
        free(reduced_net);
        bine_free_net(net);
        bine_free_intrinsic(bin);
        return -1;
    }
    uint64_t stage6_time = bine_time_ms();

    // Stage 7: CPU ELF compaction is intentionally disabled by default. The
    // optimizer's self-description parser depends on section headers, shstrtab,
    // dynsym/dynstr, and relocation sections for subsequent info/verify/optimize
    // passes. Stripping them made optimized outputs executable but no longer
    // re-analyzable by BINE itself. Keep direct cubin compaction in api_cubin.c.
    if (getenv("BINE_ENABLE_ELF_COMPACT")) {
        int compact_rc = bine_compact_elf(output_path, output_path);
        if (compact_rc == 0) {
            fprintf(stderr, "[BINE-COMPACT] ELF compaction complete\n");
        } else {
            fprintf(stderr, "[BINE-COMPACT] ELF compaction skipped (non-fatal)\n");
        }
    } else {
        fprintf(stderr, "[BINE-COMPACT] CPU ELF compaction skipped (set BINE_ENABLE_ELF_COMPACT=1 to force)\n");
    }
    uint64_t stage7_time = bine_time_ms();

    // Cleanup
    free(schedule);
    free(reduced_net);
    bine_free_net(net);
    bine_free_intrinsic(bin);

    uint64_t total_time = bine_time_ms() - start_time;

    // Report timing breakdown
    fprintf(stderr, "[BINE] Optimization complete in %lu ms\n", (unsigned long)total_time);
    fprintf(stderr, "  Stage 1 (Intrinsic): %lu ms\n", (unsigned long)(stage1_time - start_time));
    fprintf(stderr, "  Stage 2 (STRAND):    %lu ms\n", (unsigned long)(stage2_time - stage1_time));
    fprintf(stderr, "  Stage 2b (Peephole): %lu ms\n", (unsigned long)(stage2b_time - stage2_time));
    fprintf(stderr, "  Stage 3 (Net):       %lu ms\n", (unsigned long)(stage3_time - stage2b_time));
    fprintf(stderr, "  Stage 4 (ICOR):      %lu ms\n", (unsigned long)(stage4_time - stage3_time));
    fprintf(stderr, "  Stage 5 (STRAND-P):  %lu ms\n", (unsigned long)(stage5_time - stage4_time));
    fprintf(stderr, "  Stage 6 (Serialize): %lu ms\n", (unsigned long)(stage6_time - stage5_time));
    fprintf(stderr, "  Stage 7 (Compact):   %lu ms\n", (unsigned long)(stage7_time - stage6_time));

    return 0;
}

// Verification function
int bine_verify(const char* original_path, const char* optimized_path,
                const char* test_input) {
    char cmd[2048];
    char* output_orig = NULL;
    char* output_opt = NULL;
    size_t len_orig = 0, len_opt = 0;
    int exit_orig = -1, exit_opt = -1;
    int ok = -1;

    bine_chmod_exec(original_path);
    bine_chmod_exec(optimized_path);

    // Run original binary and capture output
    uint64_t time_orig_start = bine_time_ms();
    snprintf(cmd, sizeof(cmd), "\"%s\" %s 2>&1", original_path,
             test_input ? test_input : "");
    if (bine_run_capture(cmd, &output_orig, &len_orig, &exit_orig) != 0) {
        goto done;
    }
    uint64_t time_orig = bine_time_ms() - time_orig_start;

    // Run optimized binary and capture output
    uint64_t time_opt_start = bine_time_ms();
    snprintf(cmd, sizeof(cmd), "\"%s\" %s 2>&1", optimized_path,
             test_input ? test_input : "");
    if (bine_run_capture(cmd, &output_opt, &len_opt, &exit_opt) != 0) {
        goto done;
    }
    uint64_t time_opt = bine_time_ms() - time_opt_start;

    // Compare outputs
    int output_match = 0;
    if (output_orig && output_opt) {
        output_match = (len_orig == len_opt && memcmp(output_orig, output_opt, len_orig) == 0);
    } else if (!output_orig && !output_opt) {
        output_match = 1;
    }

    int exit_match = (exit_orig == exit_opt);

    printf("  Original exit code: %d\n", exit_orig);
    printf("  Optimized exit code: %d\n", exit_opt);
    printf("  Exit codes match: %s\n", exit_match ? "YES" : "NO");
    printf("  Outputs match: %s\n", output_match ? "YES" : "NO");

    if (output_orig) printf("  Original output length: %zu\n", len_orig);
    if (output_opt) printf("  Optimized output length: %zu\n", len_opt);

    // Speed comparison
    printf("  Original execution time: %lu ms\n", (unsigned long)time_orig);
    printf("  Optimized execution time: %lu ms\n", (unsigned long)time_opt);
    if (time_orig > 0 && time_opt > 0) {
        if (time_opt < time_orig) {
            double speedup = (double)time_orig / (double)time_opt;
            printf("  Speed improvement: %.2fx faster\n", speedup);
        } else if (time_opt > time_orig) {
            printf("  Speed change: optimized is slower (may need more iterations)\n");
        } else {
            printf("  Speed: equivalent execution time\n");
        }
    }

    if (exit_match && output_match) {
        ok = 0;
    }

done:
    free(output_orig);
    free(output_opt);
    return ok;
}

// Info function
int bine_info(const char* binary_path) {
    BinaryIntrinsic* bin = bine_extract_intrinsic(binary_path);
    if (!bin) {
        return -1;
    }

    printf("\n[BINE] Binary Information:\n");
    printf("  Architecture: %s\n",
           bin->e_machine == EM_X86_64 ? "x86-64" :
           bin->e_machine == EM_AARCH64 ? "AArch64" :
           bin->e_machine == EM_RISCV ? "RISC-V" : "Unknown");
    printf("  Entry point: 0x%llx\n", (unsigned long long)bin->e_entry);
    printf("  Load address: 0x%llx\n", (unsigned long long)bin->load_vaddr);
    printf("  .text size: %llu bytes\n", (unsigned long long)bin->text_size);
    printf("  .rodata size: %llu bytes\n", (unsigned long long)bin->rodata_size);
    printf("  .data size: %llu bytes\n", (unsigned long long)bin->data_size);
    printf("  Symbols: %u\n", bin->n_symbols);
    printf("  Relocations: %u\n", bin->n_relocs);
    printf("  Needed libraries: %u\n", bin->n_needed);
    printf("  Fingerprint: 0x%llx\n", (unsigned long long)bin->fingerprint);
    printf("  Build ID: %s\n", bin->has_build_id ? "present" : "absent");
    printf("  Compiler: %s\n", bin->compiler_str ? bin->compiler_str : "unknown");

    bine_free_intrinsic(bin);
    return 0;
}

// Benchmark function
int bine_bench(const char* input_path, const char* output_path) {
    uint64_t times[3];
    uint64_t size_reductions[3];

    for (int i = 0; i < 3; i++) {
        fprintf(stderr, "[BINE-BENCH] Iteration %d/3...\n", i + 1);
        uint64_t start = bine_time_ms();

        if (bine_optimize(input_path, output_path) < 0) {
            return -1;
        }

        times[i] = bine_time_ms() - start;

        // Get file sizes
        uint8_t* buf = NULL;
        size_t len = 0;
        if (bine_read_file(input_path, &buf, &len) != 0) return -1;
        free(buf);
        uint64_t orig_size = len;
        if (bine_read_file(output_path, &buf, &len) != 0) return -1;
        free(buf);
        uint64_t opt_size = len;

        size_reductions[i] = (orig_size > opt_size)
                             ? (orig_size - opt_size) * 100 / orig_size : 0;
    }

    // Calculate statistics
    uint64_t total_time = 0;
    uint64_t total_size_reduction = 0;
    uint64_t min_time = UINT64_MAX;
    uint64_t max_time = 0;

    for (int i = 0; i < 3; i++) {
        total_time += times[i];
        total_size_reduction += size_reductions[i];
        if (times[i] < min_time) min_time = times[i];
        if (times[i] > max_time) max_time = times[i];
    }

    uint64_t avg_time = total_time / 3;
    uint64_t avg_size_reduction = total_size_reduction / 3;
    uint64_t median_time = times[1];
    uint64_t median_size_reduction = size_reductions[1];

    printf("  Time:\n");
    printf("    Min: %lu ms\n", (unsigned long)min_time);
    printf("    Max: %lu ms\n", (unsigned long)max_time);
    printf("    Avg: %lu ms\n", (unsigned long)avg_time);
    printf("    Median: %lu ms\n", (unsigned long)median_time);
    printf("  Size reduction:\n");
    printf("    Avg: %lu%%\n", (unsigned long)avg_size_reduction);
    printf("    Median: %lu%%\n", (unsigned long)median_size_reduction);
    printf("  Throughput: %.2f MB/s\n",
           median_time > 0 ? (float)median_time / 1000.0f : 0.0f);

    return 0;
}
