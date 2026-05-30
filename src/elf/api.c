// api/api.c — BINE API implementation
// Main API functions: bine_optimize, bine_verify, bine_info, bine_bench

#include "bine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <elf.h>

// Helper: get current time in milliseconds
static uint64_t get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// External CUDA wrapper functions
extern int bine_gpu_icor_reduce_wrapper(const uint64_t* net, uint32_t n_agents,
                                         uint64_t** reduced_net, uint32_t* n_reduced,
                                         uint32_t* rounds);
extern int bine_strand_p_optimize_wrapper(const uint64_t* net, uint32_t n_agents,
                                           const uint8_t* masks_r, const uint8_t* masks_w,
                                           const uint8_t* masks_k,
                                           uint64_t** scheduled_net, uint32_t* n_scheduled);

uint64_t* bine_icor_reduce(const uint64_t* net, uint32_t n_agents,
                          uint32_t* n_reduced, uint32_t* rounds) {
    
    uint64_t* reduced_net = NULL;
    int result = bine_gpu_icor_reduce_wrapper(net, n_agents, &reduced_net, n_reduced, rounds);
    
    if (result != 0) {
        fprintf(stderr, "[BINE-ICOR] GPU-ICOR reduction failed\n");
        return NULL;
    }
    
    return reduced_net;
}

uint64_t* bine_strand_p_optimize(const uint64_t* net, uint32_t n_agents,
                                 const StrandMasks* masks,
                                 uint32_t* n_scheduled) {
    
    // Extract masks for scheduling
    uint8_t* masks_r = (uint8_t*)malloc(n_agents * sizeof(uint8_t));
    uint8_t* masks_w = (uint8_t*)malloc(n_agents * sizeof(uint8_t));
    uint8_t* masks_k = (uint8_t*)malloc(n_agents * sizeof(uint8_t));
    
    // Simplified mask extraction from StrandMasks
    for (uint32_t i = 0; i < n_agents; i++) {
        masks_r[i] = (masks->M_R >> (i % 64)) & 0xFF;
        masks_w[i] = (masks->M_W >> (i % 64)) & 0xFF;
        masks_k[i] = (masks->M_K >> (i % 64)) & 0xFF;
    }
    
    uint64_t* scheduled_net = NULL;
    int result = bine_strand_p_optimize_wrapper(net, n_agents, masks_r, masks_w, masks_k,
                                                &scheduled_net, n_scheduled);
    
    free(masks_r);
    free(masks_w);
    free(masks_k);
    
    if (result != 0) {
        fprintf(stderr, "[BINE-STRAND-P] STRAND-P optimization failed\n");
        return NULL;
    }
    
    return scheduled_net;
}

// Main optimization function
int bine_optimize(const char* input_path, const char* output_path) {
    
    uint64_t start_time = get_time_ms();
    
    // Stage 1: Extract intrinsic structure
    BinaryIntrinsic* bin = bine_extract_intrinsic(input_path);
    if (!bin) {
        return -1;
    }
    
    uint64_t stage1_time = get_time_ms();
    
    // Threshold-based strategy selection
    // Note: CPU ELF pipeline does not preserve instruction index mapping during
    // GPU-ICOR/STRAND-P reordering, so we disable them to prevent segfaults.
    // Peephole optimization and ELF compaction provide the main safe size/speed benefits.
    bool use_gpu_icor = true;

    // Cache lookup is disabled until cached nets can be serialized with the
    // current binary layout. Returning success on a cache hit without writing
    // output makes the CLI report false positives for repeated optimizations.
    
    // Stage 2: Derive STRAND masks
    StrandMasks masks = bine_derive_masks(bin->text_bytes, bin->text_size);
    uint64_t stage2_time = get_time_ms();
    
    // Stage 2.5: Peephole optimization on raw .text bytes
    // Runs before net construction to clean up the instruction stream:
    // eliminates redundant MOVs, dead PUSH/POP pairs, dead register loads,
    // and consolidates scattered NOPs into optimal multi-byte NOPs.
    {
        uint64_t peep_new_size = 0;
        int peep_rc = bine_peephole_optimize(bin->text_bytes, bin->text_size, &peep_new_size);
        if (peep_rc == 0) {
            fprintf(stderr, "[BINE-PEEPHOLE] Peephole pass complete on %lu byte .text\n",
                    bin->text_size);
        }
    }
    uint64_t stage2b_time = get_time_ms();
    
    // Stage 3: Build interaction net
    uint32_t n_agents = 0;
    uint32_t root_wire = 0;
    uint64_t* net = bine_build_net(bin, &masks, &n_agents, &root_wire);
    if (!net) {
        bine_free_intrinsic(bin);
        return -1;
    }
    
    uint64_t stage3_time = get_time_ms();
    
    // Stage 4: GPU-ICOR reduction (skip for small binaries)
    uint32_t n_reduced = 0;
    uint32_t icor_rounds = 0;
    uint64_t* reduced_net = NULL;
    
    if (use_gpu_icor) {
        reduced_net = bine_icor_reduce(net, n_agents, &n_reduced, &icor_rounds);
        if (!reduced_net) {
            bine_free_net(net);
            bine_free_intrinsic(bin);
            return -1;
        }
    } else {
        // Lightweight: use original net without GPU-ICOR
        reduced_net = net;
        n_reduced = n_agents;
    }
    
    uint64_t stage4_time = get_time_ms();
    
    // Stage 5: STRAND-P optimization (skip if all agents are ERA or lightweight mode)
    uint32_t n_scheduled = 0;
    uint64_t* scheduled_net = NULL;
    
    if (use_gpu_icor && n_reduced > 0) {
        scheduled_net = bine_strand_p_optimize(reduced_net, n_reduced, &masks, &n_scheduled);
        if (!scheduled_net) {
            if (use_gpu_icor) free(reduced_net);
            else bine_free_net(net);
            bine_free_intrinsic(bin);
            return -1;
        }
    } else {
        // Lightweight or all ERA - use reduced_net directly
        scheduled_net = reduced_net;
        n_scheduled = n_agents;
    }
    
    uint64_t stage5_time = get_time_ms();
    
    // Stage 6: Serialize to ELF
    if (bine_serialize_elf(scheduled_net, n_scheduled, bin, output_path) < 0) {
        if (scheduled_net != reduced_net) free(scheduled_net);
        free(reduced_net);
        bine_free_net(net);
        bine_free_intrinsic(bin);
        return -1;
    }
    
    uint64_t stage6_time = get_time_ms();
    
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
    
    uint64_t stage7_time = get_time_ms();
    
    // Cache store is paired with the disabled lookup above.
    
    // Cleanup
    if (scheduled_net != reduced_net) free(scheduled_net);
    if (use_gpu_icor) {
        free(reduced_net);
        bine_free_net(net);
    } else {
        // Lightweight mode: reduced_net is the same as net
        bine_free_net(net);
    }
    bine_free_intrinsic(bin);
    
    uint64_t total_time = get_time_ms() - start_time;
    
    // Report timing breakdown
    fprintf(stderr, "[BINE] Optimization complete in %lu ms\n", total_time);
    fprintf(stderr, "  Stage 1 (Intrinsic): %lu ms\n", stage1_time - start_time);
    fprintf(stderr, "  Stage 2 (STRAND):    %lu ms\n", stage2_time - stage1_time);
    fprintf(stderr, "  Stage 2b (Peephole): %lu ms\n", stage2b_time - stage2_time);
    fprintf(stderr, "  Stage 3 (Net):       %lu ms\n", stage3_time - stage2b_time);
    fprintf(stderr, "  Stage 4 (GPU-ICOR):  %lu ms\n", stage4_time - stage3_time);
    fprintf(stderr, "  Stage 5 (STRAND-P):  %lu ms\n", stage5_time - stage4_time);
    fprintf(stderr, "  Stage 6 (Serialize): %lu ms\n", stage6_time - stage5_time);
    fprintf(stderr, "  Stage 7 (Compact):   %lu ms\n", stage7_time - stage6_time);
    
    return 0;
}

// Verification function
int bine_verify(const char* original_path, const char* optimized_path,
                const char* test_input) {
    
    // Check if both files exist
    struct stat st_orig, st_opt;
    if (stat(original_path, &st_orig) < 0) {
        return -1;
    }
    if (stat(optimized_path, &st_opt) < 0) {
        return -1;
    }

    // Make binaries executable
    chmod(original_path, 0755);
    chmod(optimized_path, 0755);
    
    // Run original binary and capture output
    uint64_t time_orig_start = get_time_ms();
    char cmd_orig[1024];
    snprintf(cmd_orig, sizeof(cmd_orig), "%s %s 2>&1", original_path, 
             test_input ? test_input : "");
    
    FILE* fp_orig = popen(cmd_orig, "r");
    if (!fp_orig) {
        return -1;
    }
    
    char* output_orig = NULL;
    size_t len_orig = 0;
    ssize_t bytes_read;
    while ((bytes_read = getline(&output_orig, &len_orig, fp_orig)) != -1) {
        // Accumulate output
    }
    int exit_orig = pclose(fp_orig);
    uint64_t time_orig = get_time_ms() - time_orig_start;
    
    // Run optimized binary and capture output
    uint64_t time_opt_start = get_time_ms();
    char cmd_opt[1024];
    snprintf(cmd_opt, sizeof(cmd_opt), "%s %s 2>&1", optimized_path, 
             test_input ? test_input : "");
    
    FILE* fp_opt = popen(cmd_opt, "r");
    if (!fp_opt) {
        if (output_orig) free(output_orig);
        return -1;
    }
    
    char* output_opt = NULL;
    size_t len_opt = 0;
    while ((bytes_read = getline(&output_opt, &len_opt, fp_opt)) != -1) {
        // Accumulate output
    }
    int exit_opt = pclose(fp_opt);
    uint64_t time_opt = get_time_ms() - time_opt_start;
    
    // Compare outputs
    int output_match = 0;
    if (output_orig && output_opt) {
        output_match = (strcmp(output_orig, output_opt) == 0);
    } else if (!output_orig && !output_opt) {
        output_match = 1;
    }
    
    // Compare exit codes (check signal status vs normal exit)
    int exit_match = (WIFEXITED(exit_orig) && WIFEXITED(exit_opt) &&
                      WEXITSTATUS(exit_orig) == WEXITSTATUS(exit_opt));
    
    printf("  Original exit code: %d\n", WIFEXITED(exit_orig) ? WEXITSTATUS(exit_orig) : -1);
    printf("  Optimized exit code: %d\n", WIFEXITED(exit_opt) ? WEXITSTATUS(exit_opt) : -1);
    printf("  Exit codes match: %s\n", exit_match ? "YES" : "NO");
    printf("  Outputs match: %s\n", output_match ? "YES" : "NO");
    
    if (output_orig) {
        printf("  Original output length: %zu\n", strlen(output_orig));
        free(output_orig);
    }
    if (output_opt) {
        printf("  Optimized output length: %zu\n", strlen(output_opt));
        free(output_opt);
    }

    // Speed comparison
    printf("  Original execution time: %lu ms\n", time_orig);
    printf("  Optimized execution time: %lu ms\n", time_opt);
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
        return 0;
    } else {
        return -1;
    }
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
    printf("  Entry point: 0x%lx\n", bin->e_entry);
    printf("  Load address: 0x%lx\n", bin->load_vaddr);
    printf("  .text size: %lu bytes\n", bin->text_size);
    printf("  .rodata size: %lu bytes\n", bin->rodata_size);
    printf("  .data size: %lu bytes\n", bin->data_size);
    printf("  Symbols: %u\n", bin->n_symbols);
    printf("  Relocations: %u\n", bin->n_relocs);
    printf("  Needed libraries: %u\n", bin->n_needed);
    printf("  Fingerprint: 0x%lx\n", bin->fingerprint);
    printf("  Build ID: %s\n", bin->has_build_id ? "present" : "absent");
    printf("  Compiler: %s\n", bin->compiler_str ? bin->compiler_str : "unknown");
    
    bine_free_intrinsic(bin);
    return 0;
}

// Benchmark function
int bine_bench(const char* input_path, const char* output_path) {
    
    uint64_t times[5];
    uint64_t size_reductions[5];
    uint64_t text_reductions[5];
    
    for (int i = 0; i < 5; i++) {
        uint64_t start = get_time_ms();
        
        if (bine_optimize(input_path, output_path) < 0) {
            return -1;
        }
        
        times[i] = get_time_ms() - start;
        
        // Get file sizes
        struct stat st_orig, st_opt;
        stat(input_path, &st_orig);
        stat(output_path, &st_opt);
        
        size_reductions[i] = (st_orig.st_size - st_opt.st_size) * 100 / st_orig.st_size;
        
        // Get .text section sizes for more detailed metrics
        BinaryIntrinsic* bin_orig = bine_extract_intrinsic(input_path);
        BinaryIntrinsic* bin_opt = bine_extract_intrinsic(output_path);
        
        if (bin_orig && bin_opt) {
            text_reductions[i] = (bin_orig->text_size - bin_opt->text_size) * 100 / bin_orig->text_size;
        } else {
            text_reductions[i] = size_reductions[i]; // Fallback
        }
        
        if (bin_orig) bine_free_intrinsic(bin_orig);
        if (bin_opt) bine_free_intrinsic(bin_opt);
    }
    
    // Calculate statistics
    uint64_t total_time = 0;
    uint64_t total_size_reduction = 0;
    uint64_t total_text_reduction = 0;
    uint64_t min_time = UINT64_MAX;
    uint64_t max_time = 0;
    
    for (int i = 0; i < 5; i++) {
        total_time += times[i];
        total_size_reduction += size_reductions[i];
        total_text_reduction += text_reductions[i];
        if (times[i] < min_time) min_time = times[i];
        if (times[i] > max_time) max_time = times[i];
    }
    
    uint64_t avg_time = total_time / 5;
    uint64_t avg_size_reduction = total_size_reduction / 5;
    uint64_t avg_text_reduction = total_text_reduction / 5;
    
    // Simple median for 5 values: index 2
    uint64_t median_time = times[2];
    uint64_t median_size_reduction = size_reductions[2];
    uint64_t median_text_reduction = text_reductions[2];
    
    printf("  Time:\n");
    printf("    Min: %lu ms\n", min_time);
    printf("    Max: %lu ms\n", max_time);
    printf("    Avg: %lu ms\n", avg_time);
    printf("    Median: %lu ms\n", median_time);
    printf("  Size reduction:\n");
    printf("    Avg: %lu%%\n", avg_size_reduction);
    printf("    Median: %lu%%\n", median_size_reduction);
    printf("  .text reduction:\n");
    printf("    Avg: %lu%%\n", avg_text_reduction);
    printf("    Median: %lu%%\n", median_text_reduction);
    printf("  Throughput: %.2f MB/s\n",
           (float)median_time / 1000.0f > 0 ? 
           (float)avg_size_reduction / (float)median_time : 0.0f);
    
    return 0;
}
