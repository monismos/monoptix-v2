// bine/net/gpu_cfg.c ??? GPU Control Flow Graph Construction
// Builds CFG for GPU kernels (PTX-decoded instructions); SIMT-aware reachability is conservative
// when branch targets cannot be resolved.

#include "bine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t gpu_kernel_insn_base(const GPUIntrinsic* gpu, uint32_t kernel_idx) {
    uint64_t base = 0;
    for (uint32_t i = 0; i < kernel_idx; i++) {
        base += (uint64_t)gpu->kernels[i].insn_count;
    }
    return base;
}

/* Count basic blocks (maximal sequential segments ending at branch/ret). */
static uint32_t gpu_count_blocks_in_range(const Instruction* insns, uint64_t base, uint64_t lim) {
    uint32_t bc = 0;
    uint64_t seg_start = base;
    while (seg_start < lim) {
        bc++;
        uint64_t i = seg_start;
        while (i < lim && !insns[i].is_branch && !insns[i].is_ret) i++;
        if (i >= lim) break;
        seg_start = i + 1;
    }
    return bc;
}

static int gpu_append_kernel_blocks(const GPUIntrinsic* gpu, uint32_t kernel_idx,
                                    const Instruction* insns, uint32_t n_insns,
                                    GPUCFG* cfg) {
    uint64_t base = gpu_kernel_insn_base(gpu, kernel_idx);
    uint64_t lim = base + (uint64_t)gpu->kernels[kernel_idx].insn_count;
    if (lim > n_insns) lim = n_insns;

    cfg->kernel_start[kernel_idx] = cfg->n_blocks;

    if (base >= lim || base >= (uint64_t)n_insns) {
        return 0;
    }

    uint32_t bc = gpu_count_blocks_in_range(insns, base, lim);
    uint32_t block_start = cfg->n_blocks;

    GPUCFG tmp = *cfg;
    tmp.blocks = realloc(cfg->blocks, (cfg->n_blocks + bc) * sizeof(*cfg->blocks));
    if (!tmp.blocks) return -1;
    cfg->blocks = tmp.blocks;

    for (uint32_t i = 0; i < bc; i++) {
        memset(&cfg->blocks[cfg->n_blocks + i], 0, sizeof(cfg->blocks[0]));
        cfg->blocks[cfg->n_blocks + i].kernel_id = kernel_idx;
    }

    uint32_t b = block_start;
    uint64_t seg_start = base;

    while (seg_start < lim) {
        uint64_t i = seg_start;
        while (i < lim && !insns[i].is_branch && !insns[i].is_ret) i++;

        if (i >= lim) {
            cfg->blocks[b].start_insn = seg_start;
            cfg->blocks[b].end_insn = lim - 1;
            cfg->blocks[b].kernel_id = kernel_idx;
            cfg->blocks[b].is_entry = (b == block_start);
            cfg->blocks[b].is_exit = 1;
            b++;
            break;
        }

        cfg->blocks[b].start_insn = seg_start;
        cfg->blocks[b].end_insn = i;
        cfg->blocks[b].kernel_id = kernel_idx;
        cfg->blocks[b].is_entry = (b == block_start);
        cfg->blocks[b].is_exit = insns[i].is_ret ? (uint8_t)1 : (uint8_t)0;
        b++;
        seg_start = i + 1;
    }

    cfg->n_blocks = b;
    return 0;
}

static void gpu_fill_insn_to_block(const GPUCFG* cfg, uint32_t n_insns, uint32_t* map) {
    for (uint32_t i = 0; i < n_insns; i++) {
        map[i] = UINT32_MAX;
    }
    for (uint32_t bi = 0; bi < cfg->n_blocks; bi++) {
        uint64_t lo = cfg->blocks[bi].start_insn;
        uint64_t hi = cfg->blocks[bi].end_insn;
        if (lo >= (uint64_t)n_insns) continue;
        if (hi >= (uint64_t)n_insns) hi = (uint64_t)n_insns - 1;
        for (uint64_t i = lo; i <= hi; i++) {
            map[(uint32_t)i] = bi;
        }
    }
}

static void gpu_wire_successors(GPUCFG* cfg, const Instruction* insns, uint32_t n_insns,
                               const GPUIntrinsic* gpu, const uint32_t* insn_to_block) {
    for (uint32_t bi = 0; bi < cfg->n_blocks; bi++) {
        free(cfg->blocks[bi].successors);
        cfg->blocks[bi].successors = NULL;
        cfg->blocks[bi].n_succ = 0;

        uint64_t last = cfg->blocks[bi].end_insn;
        if (last >= (uint64_t)n_insns) continue;

        uint32_t kid = cfg->blocks[bi].kernel_id;
        uint64_t kbase = gpu_kernel_insn_base(gpu, kid);
        uint64_t klim = kbase + (uint64_t)gpu->kernels[kid].insn_count;
        if (klim > (uint64_t)n_insns) klim = (uint64_t)n_insns;

        if (insns[last].is_ret) {
            continue;
        }

        if (!insns[last].is_branch) {
            if (last + 1 < klim && last + 1 < (uint64_t)n_insns) {
                uint32_t ft = insn_to_block[(uint32_t)(last + 1)];
                if (ft != UINT32_MAX) {
                    cfg->blocks[bi].n_succ = 1;
                    cfg->blocks[bi].successors = (uint32_t*)malloc(sizeof(uint32_t));
                    cfg->blocks[bi].successors[0] = ft;
                }
            }
            continue;
        }

        uint64_t tgt_insn = insns[last].target;
        uint32_t tgt_block = UINT32_MAX;
        if (tgt_insn != UINT64_MAX && tgt_insn < (uint64_t)n_insns) {
            tgt_block = insn_to_block[(uint32_t)tgt_insn];
        }

        bool unconditional = !insns[last].predicated &&
                             strstr((const char*)insns[last].opcode, "uni") != NULL;

        uint32_t ft_block = UINT32_MAX;
        if (last + 1 < klim && last + 1 < (uint64_t)n_insns) {
            ft_block = insn_to_block[(uint32_t)(last + 1)];
        }

        if (unconditional) {
            cfg->blocks[bi].n_succ = 1;
            cfg->blocks[bi].successors = (uint32_t*)malloc(sizeof(uint32_t));
            cfg->blocks[bi].successors[0] = tgt_block;
        } else {
            uint32_t n_succ = 0;
            if (tgt_block != UINT32_MAX) n_succ++;
            if (ft_block != UINT32_MAX) n_succ++;
            cfg->blocks[bi].n_succ = n_succ;
            if (n_succ == 0) continue;
            cfg->blocks[bi].successors = (uint32_t*)malloc(n_succ * sizeof(uint32_t));
            uint32_t p = 0;
            if (tgt_block != UINT32_MAX) cfg->blocks[bi].successors[p++] = tgt_block;
            if (ft_block != UINT32_MAX) cfg->blocks[bi].successors[p++] = ft_block;
        }
    }
}

static void gpu_build_predecessors(GPUCFG* cfg) {
    for (uint32_t i = 0; i < cfg->n_blocks; i++) {
        free(cfg->blocks[i].predecessors);
        cfg->blocks[i].predecessors = NULL;
        cfg->blocks[i].n_pred = 0;
    }

    for (uint32_t i = 0; i < cfg->n_blocks; i++) {
        GPUCFGBlock* block = &cfg->blocks[i];

        uint32_t pred_count = 0;
        for (uint32_t j = 0; j < cfg->n_blocks; j++) {
            GPUCFGBlock* other = &cfg->blocks[j];
            for (uint32_t k = 0; k < other->n_succ; k++) {
                if (other->successors[k] == i) pred_count++;
            }
        }

        block->n_pred = pred_count;
        if (pred_count > 0) {
            block->predecessors = (uint32_t*)malloc(pred_count * sizeof(uint32_t));
            uint32_t pred_idx = 0;

            for (uint32_t j = 0; j < cfg->n_blocks; j++) {
                GPUCFGBlock* other = &cfg->blocks[j];
                for (uint32_t k = 0; k < other->n_succ; k++) {
                    if (other->successors[k] == i) {
                        block->predecessors[pred_idx++] = j;
                    }
                }
            }
        }
    }
}

GPUCFG* bine_build_gpu_cfg(const GPUIntrinsic* gpu,
                          const Instruction* insns, uint32_t n_insns) {
    if (!gpu || !insns) return NULL;

    GPUCFG* cfg = (GPUCFG*)calloc(1, sizeof(GPUCFG));
    if (!cfg) return NULL;

    cfg->n_kernels = gpu->n_kernels;
    cfg->kernel_start = (uint32_t*)calloc(gpu->n_kernels, sizeof(uint32_t));
    if (!cfg->kernel_start) {
        free(cfg);
        return NULL;
    }

    for (uint32_t i = 0; i < gpu->n_kernels; i++) {
        if (gpu_append_kernel_blocks(gpu, i, insns, n_insns, cfg) < 0) {
            bine_free_gpu_cfg(cfg);
            return NULL;
        }
    }

    uint32_t* insn_to_block = (uint32_t*)malloc((size_t)n_insns * sizeof(uint32_t));
    if (!insn_to_block) {
        bine_free_gpu_cfg(cfg);
        return NULL;
    }

    gpu_fill_insn_to_block(cfg, n_insns, insn_to_block);
    gpu_wire_successors(cfg, insns, n_insns, gpu, insn_to_block);
    gpu_build_predecessors(cfg);

    free(insn_to_block);

    printf("[GPU-CFG] Built CFG with %u blocks across %u kernels\n",
           cfg->n_blocks, cfg->n_kernels);

    return cfg;
}

void bine_free_gpu_cfg(GPUCFG* cfg) {
    if (!cfg) return;

    if (cfg->blocks) {
        for (uint32_t i = 0; i < cfg->n_blocks; i++) {
            free(cfg->blocks[i].successors);
            free(cfg->blocks[i].predecessors);
        }
        free(cfg->blocks);
    }

    free(cfg->kernel_start);
    free(cfg);
}

int bine_gpu_analyze_reachability(const GPUCFG* cfg, bool* reachable) {
    if (!cfg || !reachable) return -1;

    uint32_t n = cfg->n_blocks;
    memset(reachable, 0, (size_t)n * sizeof(bool));

    uint32_t* queue = (uint32_t*)malloc((size_t)n * sizeof(uint32_t));
    if (!queue) return -1;
    uint32_t qh = 0;
    uint32_t qt = 0;

    for (uint32_t k = 0; k < cfg->n_kernels; k++) {
        uint32_t eb = cfg->kernel_start[k];
        if (eb < n && !reachable[eb]) {
            reachable[eb] = true;
            queue[qt++] = eb;
        }
    }

    while (qh < qt) {
        uint32_t b = queue[qh++];
        const GPUCFGBlock* blk = &cfg->blocks[b];
        for (uint32_t si = 0; si < blk->n_succ; si++) {
            uint32_t s = blk->successors[si];
            if (s != UINT32_MAX && s < n && !reachable[s]) {
                reachable[s] = true;
                queue[qt++] = s;
            }
        }
    }

    uint8_t* kernel_unknown = (uint8_t*)calloc(cfg->n_kernels, 1);
    if (!kernel_unknown) {
        free(queue);
        return -1;
    }

    for (uint32_t b = 0; b < n; b++) {
        if (!reachable[b]) continue;
        uint32_t kid = cfg->blocks[b].kernel_id;
        for (uint32_t si = 0; si < cfg->blocks[b].n_succ; si++) {
            if (cfg->blocks[b].successors[si] == UINT32_MAX) {
                kernel_unknown[kid] = 1;
            }
        }
    }

    for (uint32_t k = 0; k < cfg->n_kernels; k++) {
        if (!kernel_unknown[k]) continue;
        uint32_t lo = cfg->kernel_start[k];
        uint32_t hi = (k + 1 < cfg->n_kernels) ? cfg->kernel_start[k + 1] : n;
        if (hi > n) hi = n;
        for (uint32_t b = lo; b < hi; b++) {
            reachable[b] = true;
        }
    }

    uint32_t reachable_count = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (reachable[i]) reachable_count++;
    }

    printf("[GPU-CFG] Reachability: %u/%u blocks marked reachable\n", reachable_count, n);

    free(kernel_unknown);
    free(queue);
    return 0;
}

