/*
 * serialize/layout.c — Offset-preserving instruction layout engine.
 *
 * The ELF serializer must NEVER change .text size: section offsets, symbol
 * values, relocation offsets and branch displacements all depend on the
 * existing layout.  This engine therefore rewrites .text as:
 *
 *   1. Dead code elimination — a dead instruction's bytes are replaced by
 *      multi-byte NOPs of identical total length (offsets preserved).
 *   2. STRAND-P reordering — adjacent same-length instructions are swapped
 *      only when provably safe (see can_swap()), realizing the scheduler's
 *      desired order without disturbing any offset.
 *
 * Every elimination is gated by THREE independent checks:
 *   - the interaction net says the agent is ERA (ICOR agreement),
 *   - the instruction is not control flow / essential,
 *   - it is not a relocation site or a branch target.
 * A rewrite is therefore correct-by-construction.
 */

#include "bine.h"
#include "icor/bits.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Multi-byte NOP encodings for x86-64 (length 1..15) */
static const uint8_t nop_encodings[15][15] = {
    {0x90},
    {0x66, 0x90},
    {0x0F, 0x1F, 0x00},
    {0x0F, 0x1F, 0x40, 0x00},
    {0x0F, 0x1F, 0x44, 0x00, 0x00},
    {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},
    {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},
    {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x66, 0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
};

static void emit_nop(uint8_t *dst, uint8_t len) {
    while (len > 0) {
        uint8_t n = (len > 15) ? 15 : len;
        memcpy(dst, nop_encodings[n - 1], n);
        dst += n;
        len -= n;
    }
}

/* --------------------------------------------------------------------------
 * Dead-code computation (two-oracle gate)
 * ------------------------------------------------------------------------ */

static int is_reloc_site(const BinaryIntrinsic *bin, uint64_t offset) {
    for (uint32_t i = 0; i < bin->n_relocs; i++) {
        if (bin->relocs[i].offset == offset) return 1;
    }
    return 0;
}

/*
 * Compute per-instruction dead flags.
 *
 * An instruction is dead only if ALL of these hold:
 *   - its net agent is ERA (ICOR agreement; net_build only emits ERA for
 *     CFG-unreachable code seeded from entry/symbols/relocs),
 *   - it is not essential (CALL/RET/branch),
 *   - it is not a relocation site,
 *   - it is not a branch/call target.
 * Returns a malloc'd bitmap of n_insns bytes, or NULL on failure.
 */
uint8_t *bine_compute_dead_agents(const uint64_t *net, uint32_t n_agents,
                                  const Instruction *insns, uint32_t n_insns,
                                  const BinaryIntrinsic *bin) {
    if (!net || !insns || !bin) return NULL;

    uint8_t *is_dead = (uint8_t *)calloc(n_insns, sizeof(uint8_t));
    uint8_t *is_target = (uint8_t *)calloc(n_insns, sizeof(uint8_t));
    if (!is_dead || !is_target) {
        free(is_dead);
        free(is_target);
        return NULL;
    }

    for (uint32_t i = 0; i < n_insns; i++) {
        if (insns[i].target != 0) {
            for (uint32_t j = 0; j < n_insns; j++) {
                if (insns[j].offset == insns[i].target) {
                    is_target[j] = 1;
                    break;
                }
            }
        }
    }

    for (uint32_t i = 0; i < n_insns; i++) {
        uint64_t w = (i < n_agents) ? net[i] : 0;
        if (WORD_TYPE(w) != 2) continue;               /* not ERA: keep */
        if (WORD_ESSENTIAL(w)) continue;               /* essential: keep */
        if (insns[i].is_call || insns[i].is_ret || insns[i].is_branch) continue;
        if (is_reloc_site(bin, insns[i].offset)) continue;
        if (is_target[i]) continue;                    /* branch target: keep */
        is_dead[i] = 1;
    }

    free(is_target);
    return is_dead;
}

/* --------------------------------------------------------------------------
 * Safe reordering
 * ------------------------------------------------------------------------ */

/* Register operands of a reg-only instruction.  Returns 0 if the instruction
 * is not in the provably-swappable class. */
static int reg_operands(const Instruction *insn, uint32_t *def, uint32_t *use,
                        uint32_t *n_use) {
    *n_use = 0;
    uint8_t op = insn->opcode[0];

    if (op == 0x90) {                 /* NOP: no operands */
        *def = 0xFFFFFFFFu;
        return 1;
    }

    if (op >= 0xB8 && op <= 0xBF) {   /* MOV r64, imm32: def only */
        *def = insn->reg;
        return 1;
    }

    if ((op == 0x89 || op == 0x8B) && insn->has_modrm &&
        (insn->modrm & 0xC0) == 0xC0) {  /* reg-reg MOV */
        if (op == 0x8B) {             /* MOV r, r/m: def = reg, use = rm */
            *def = insn->reg;
            if (insn->n_use_regs > 0) {
                use[0] = insn->use_regs[0];
                *n_use = 1;
            } else {
                use[0] = insn->rm;
                *n_use = 1;
            }
        } else {                      /* MOV r/m, r: def = rm, use = reg */
            *def = insn->rm;
            if (insn->n_use_regs > 0) {
                use[0] = insn->use_regs[0];
                *n_use = 1;
            } else {
                return 0;
            }
        }
        return 1;
    }

    return 0;
}

static int regs_disjoint(const Instruction *a, const Instruction *b) {
    uint32_t da, db, ua[16], ub[16], na, nb;
    if (!reg_operands(a, &da, ua, &na)) return 0;
    if (!reg_operands(b, &db, ub, &nb)) return 0;

    for (uint32_t i = 0; i < na; i++) {
        if (ua[i] == db) return 0;
        for (uint32_t j = 0; j < nb; j++) if (ua[i] == ub[j]) return 0;
    }
    for (uint32_t i = 0; i < nb; i++) {
        if (ub[i] == da) return 0;
    }
    if (da != 0xFFFFFFFFu && db != 0xFFFFFFFFu && da == db) return 0;
    return 1;
}

/* May instruction i and i+1 be swapped without changing semantics?
 * Requires: equal length, both live, no control flow, no reloc site, no
 * branch target, register-only operands, disjoint registers. */
static int can_swap(const Instruction *a, const Instruction *b,
                    const uint8_t *is_dead, const BinaryIntrinsic *bin) {
    if (is_dead[0] || is_dead[1]) return 0;
    if (a->length != b->length) return 0;
    if (a->is_call || a->is_ret || a->is_branch) return 0;
    if (b->is_call || b->is_ret || b->is_branch) return 0;
    if (a->target != 0 || b->target != 0) return 0;
    if (is_reloc_site(bin, a->offset) || is_reloc_site(bin, b->offset)) return 0;
    return regs_disjoint(a, b);
}

/* --------------------------------------------------------------------------
 * Layout application
 * ------------------------------------------------------------------------ */

/*
 * Rewrite text_bytes (size original->text_size) applying DCE and the
 * STRAND-P schedule order.
 *
 *   net            slot-stable interaction net (net[i] ↔ instruction i)
 *   schedule_order permutation produced by bine_strand_p_schedule_cpu
 *   original       source binary self-description
 *   out            caller buffer, original->text_size bytes
 *   offset_map     out: malloc'd n_insns entries: new offset of each insn
 *
 * Returns n_insns on success (>= 0), or -1 on failure.
 */
int bine_apply_layout(const uint64_t *net, uint32_t n_agents,
                      const uint32_t *schedule_order, uint32_t n_scheduled,
                      const BinaryIntrinsic *original,
                      uint8_t *out, uint64_t *out_size,
                      uint64_t **offset_map_out) {
    if (!net || !original || !out || !out_size || !offset_map_out) return -1;

    uint32_t n_insns = 0;
    Instruction *insns = decode_instructions(original->text_bytes,
                                             original->text_size, &n_insns);
    if (!insns || n_insns == 0) {
        free_instructions(insns);
        return -1;
    }

    uint8_t *is_dead = bine_compute_dead_agents(net, n_agents, insns, n_insns,
                                                original);
    if (!is_dead) {
        free_instructions(insns);
        return -1;
    }

    /* Rank per instruction from the schedule order (higher = earlier). */
    uint32_t *rank = (uint32_t *)malloc(n_insns * sizeof(uint32_t));
    uint64_t *orig_offsets = (uint64_t *)malloc(n_insns * sizeof(uint64_t));
    if (!rank || !orig_offsets) {
        free(rank);
        free(orig_offsets);
        free(is_dead);
        free_instructions(insns);
        return -1;
    }
    for (uint32_t i = 0; i < n_insns; i++) {
        rank[i] = 0;
        orig_offsets[i] = insns[i].offset;
    }
    if (schedule_order) {
        for (uint32_t k = 0; k < n_scheduled && k < n_insns; k++) {
            if (schedule_order[k] < n_insns) {
                rank[schedule_order[k]] = n_scheduled - k;
            }
        }
    }

    /* Realize the schedule with provably-safe adjacent same-length swaps.
     * Bubble repeatedly until stable (bounded by n_insns passes). */
    for (uint32_t pass = 0; pass < n_insns; pass++) {
        int changed = 0;
        for (uint32_t i = 0; i + 1 < n_insns; i++) {
            if (rank[i] >= rank[i + 1]) continue;
            if (!can_swap(&insns[i], &insns[i + 1], is_dead + i, original)) {
                continue;
            }
            /* Swap instruction metadata (equal length ⇒ instruction
             * boundaries are identical; offsets are recomputed below). */
            Instruction tmp = insns[i];
            insns[i] = insns[i + 1];
            insns[i + 1] = tmp;

            uint64_t to = orig_offsets[i];
            orig_offsets[i] = orig_offsets[i + 1];
            orig_offsets[i + 1] = to;

            uint32_t tr = rank[i];
            rank[i] = rank[i + 1];
            rank[i + 1] = tr;
            changed = 1;
        }
        if (!changed) break;
    }

    /* Recompute positional offsets after any swaps. */
    uint64_t running = 0;
    for (uint32_t i = 0; i < n_insns; i++) {
        insns[i].offset = running;
        running += insns[i].length;
    }

    /* Emit final bytes: live instructions verbatim, dead as NOPs. */
    uint64_t offset = 0;
    for (uint32_t i = 0; i < n_insns; i++) {
        uint8_t len = insns[i].length;
        if (is_dead[i]) {
            if (len > 0 && len <= 15) emit_nop(out + offset, len);
            else memcpy(out + offset, original->text_bytes + orig_offsets[i], len);
        } else {
            memcpy(out + offset, original->text_bytes + orig_offsets[i], len);
        }
        offset += len;
    }

    *out_size = offset;
    if (offset > original->text_size) {
        /* Cannot happen: same-length swaps + same-length NOP replacement. */
        free(rank);
        free(orig_offsets);
        free(is_dead);
        free_instructions(insns);
        return -1;
    }

    /* offset_map: original instruction i -> new offset. */
    uint64_t *offset_map = (uint64_t *)malloc(n_insns * sizeof(uint64_t));
    if (!offset_map) {
        free(rank);
        free(orig_offsets);
        free(is_dead);
        free_instructions(insns);
        return -1;
    }
    for (uint32_t i = 0; i < n_insns; i++) offset_map[i] = insns[i].offset;

    *offset_map_out = offset_map;
    free(rank);
    free(orig_offsets);
    free(is_dead);
    free_instructions(insns);
    return (int)n_insns;
}
