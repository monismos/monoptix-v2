/*
 * icor/reduce_cpu.c — Host-side ICOR reduction with slot-stable semantics.
 *
 * The ELF pipeline consumes net arrays where agent i corresponds 1:1 to
 * instruction i (see optimizer/net_build.c).  GPU reduction compacts and
 * renumbers agents, which loses that mapping.  This host reducer applies
 * the identical six interaction rules but NEVER renumbers live original
 * agents: dead agents are tombstoned in place and agents created by the
 * CON><DUP rule are appended.  The serializer can therefore decide
 * per-instruction liveness directly from agent slots.
 *
 * Determinism: pair selection uses a deterministic splitmix64 p-bit
 * Maximum Independent Set scheduler (same algorithm family as the GPU
 * path), so results are reproducible across platforms and runs.
 */

#include "bine.h"
#include "icor/bits.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Deterministic RNG (splitmix64) — matches strand/rng.cuh
 * ------------------------------------------------------------------------ */

typedef struct {
    uint64_t s;
} bine_rng_t;

static uint64_t rng_next(bine_rng_t *r) {
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static double rng_uniform(bine_rng_t *r) {
    return (double)(rng_next(r) >> 11) * (1.0 / 9007199254740992.0);
}

/* --------------------------------------------------------------------------
 * The six interaction rules (ported from icor/rules.cuh, pure bit ops)
 * ------------------------------------------------------------------------ */

static void rule_con_con(uint64_t *net, uint32_t a, uint32_t b) {
    uint64_t wa = net[a], wb = net[b];
    uint32_t al_a = WORD_PORT_ADDR(wa, 1); uint8_t al_p = WORD_PORT_ID(wa, 1);
    uint32_t ar_a = WORD_PORT_ADDR(wa, 2); uint8_t ar_p = WORD_PORT_ID(wa, 2);
    uint32_t bl_a = WORD_PORT_ADDR(wb, 1); uint8_t bl_p = WORD_PORT_ID(wb, 1);
    uint32_t br_a = WORD_PORT_ADDR(wb, 2); uint8_t br_p = WORD_PORT_ID(wb, 2);

    net[al_a] = SET_PORT(net[al_a], al_p, PORTREF(br_a, br_p));
    net[br_a] = SET_PORT(net[br_a], br_p, PORTREF(al_a, al_p));
    net[ar_a] = SET_PORT(net[ar_a], ar_p, PORTREF(bl_a, bl_p));
    net[bl_a] = SET_PORT(net[bl_a], bl_p, PORTREF(ar_a, ar_p));

    net[a] = SET_DEAD(wa);
    net[b] = SET_DEAD(wb);
}

static void rule_dup_dup(uint64_t *net, uint32_t a, uint32_t b) {
    uint64_t wa = net[a], wb = net[b];
    uint32_t al_a = WORD_PORT_ADDR(wa, 1); uint8_t al_p = WORD_PORT_ID(wa, 1);
    uint32_t ar_a = WORD_PORT_ADDR(wa, 2); uint8_t ar_p = WORD_PORT_ID(wa, 2);
    uint32_t bl_a = WORD_PORT_ADDR(wb, 1); uint8_t bl_p = WORD_PORT_ID(wb, 1);
    uint32_t br_a = WORD_PORT_ADDR(wb, 2); uint8_t br_p = WORD_PORT_ID(wb, 2);

    net[al_a] = SET_PORT(net[al_a], al_p, PORTREF(bl_a, bl_p));
    net[bl_a] = SET_PORT(net[bl_a], bl_p, PORTREF(al_a, al_p));
    net[ar_a] = SET_PORT(net[ar_a], ar_p, PORTREF(br_a, br_p));
    net[br_a] = SET_PORT(net[br_a], br_p, PORTREF(ar_a, ar_p));

    net[a] = SET_DEAD(wa);
    net[b] = SET_DEAD(wb);
}

static void rule_era_era(uint64_t *net, uint32_t a, uint32_t b) {
    net[a] = SET_DEAD(net[a]);
    net[b] = SET_DEAD(net[b]);
}

static void rule_x_era(uint64_t *net, uint32_t x, uint32_t era_slot,
                       uint32_t fresh_era_slot) {
    uint64_t wx = net[x];
    uint32_t l_a = WORD_PORT_ADDR(wx, 1); uint8_t l_p = WORD_PORT_ID(wx, 1);
    uint32_t r_a = WORD_PORT_ADDR(wx, 2); uint8_t r_p = WORD_PORT_ID(wx, 2);

    uint64_t era_word_l = SET_TYPE(0ULL, 2);
    era_word_l = SET_PORT(era_word_l, 0, PORTREF(l_a, l_p));
    net[era_slot] = era_word_l;
    net[l_a] = SET_PORT(net[l_a], l_p, PORTREF(era_slot, 0));

    uint64_t era_word_r = SET_TYPE(0ULL, 2);
    era_word_r = SET_PORT(era_word_r, 0, PORTREF(r_a, r_p));
    net[fresh_era_slot] = era_word_r;
    net[r_a] = SET_PORT(net[r_a], r_p, PORTREF(fresh_era_slot, 0));

    net[x] = SET_DEAD(wx);
}

static void rule_con_dup(uint64_t *net, uint32_t con, uint32_t dup,
                         uint32_t base) {
    uint64_t wc = net[con], wd = net[dup];
    uint8_t con_type = (uint8_t)WORD_TYPE(wc);
    uint8_t dup_type = (uint8_t)WORD_TYPE(wd);

    uint32_t d1 = base, d2 = base + 1, c1 = base + 2, c2 = base + 3;

    uint32_t cl_a = WORD_PORT_ADDR(wc, 1); uint8_t cl_p = WORD_PORT_ID(wc, 1);
    uint32_t cr_a = WORD_PORT_ADDR(wc, 2); uint8_t cr_p = WORD_PORT_ID(wc, 2);
    uint32_t dl_a = WORD_PORT_ADDR(wd, 1); uint8_t dl_p = WORD_PORT_ID(wd, 1);
    uint32_t dr_a = WORD_PORT_ADDR(wd, 2); uint8_t dr_p = WORD_PORT_ID(wd, 2);

    uint64_t wd1 = SET_TYPE(0ULL, dup_type);
    wd1 = SET_PORT(wd1, 0, PORTREF(cl_a, cl_p));
    wd1 = SET_PORT(wd1, 1, PORTREF(c1, 1));
    wd1 = SET_PORT(wd1, 2, PORTREF(c2, 1));
    net[d1] = wd1;

    uint64_t wd2 = SET_TYPE(0ULL, dup_type);
    wd2 = SET_PORT(wd2, 0, PORTREF(cr_a, cr_p));
    wd2 = SET_PORT(wd2, 1, PORTREF(c1, 2));
    wd2 = SET_PORT(wd2, 2, PORTREF(c2, 2));
    net[d2] = wd2;

    uint64_t wc1 = SET_TYPE(0ULL, con_type);
    wc1 = SET_PORT(wc1, 0, PORTREF(dl_a, dl_p));
    wc1 = SET_PORT(wc1, 1, PORTREF(d1, 1));
    wc1 = SET_PORT(wc1, 2, PORTREF(d2, 1));
    net[c1] = wc1;

    uint64_t wc2 = SET_TYPE(0ULL, con_type);
    wc2 = SET_PORT(wc2, 0, PORTREF(dr_a, dr_p));
    wc2 = SET_PORT(wc2, 1, PORTREF(d1, 2));
    wc2 = SET_PORT(wc2, 2, PORTREF(d2, 2));
    net[c2] = wc2;

    net[cl_a] = SET_PORT(net[cl_a], cl_p, PORTREF(d1, 0));
    net[cr_a] = SET_PORT(net[cr_a], cr_p, PORTREF(d2, 0));
    net[dl_a] = SET_PORT(net[dl_a], dl_p, PORTREF(c1, 0));
    net[dr_a] = SET_PORT(net[dr_a], dr_p, PORTREF(c2, 0));

    net[con] = SET_DEAD(wc);
    net[dup] = SET_DEAD(wd);
}

/* Apply the matching rule to an active pair (a < b).  Returns 1 if the pair
 * was consumed, 0 if a rule produced agents that must be processed later. */
static int apply_rule(uint64_t *net, uint32_t *alloc_head,
                      uint32_t max_agents, uint32_t a, uint32_t b) {
    uint64_t wa = net[a], wb = net[b];
    if (WORD_DEAD(wa) || WORD_DEAD(wb)) return 0;
    if (WORD_PORT_ADDR(wa, 0) != b || WORD_PORT_ADDR(wb, 0) != a) return 0;

    uint32_t ta = (uint32_t)WORD_TYPE(wa);
    uint32_t tb = (uint32_t)WORD_TYPE(wb);

    if (ta == 0 && tb == 0) { rule_con_con(net, a, b); return 1; }
    if (ta == 1 && tb == 1) { rule_dup_dup(net, a, b); return 1; }
    if (ta == 2 && tb == 2) { rule_era_era(net, a, b); return 1; }
    if ((ta == 0 || ta == 1) && tb == 2) {
        if (*alloc_head + 1 >= max_agents) return 0;
        rule_x_era(net, a, b, (*alloc_head)++);
        return 1;
    }
    if (ta == 2 && (tb == 0 || tb == 1)) {
        if (*alloc_head + 1 >= max_agents) return 0;
        rule_x_era(net, b, a, (*alloc_head)++);
        return 1;
    }
    if ((ta == 0 && tb == 1) || (ta == 1 && tb == 0)) {
        if (*alloc_head + 3 >= max_agents) return 0;
        rule_con_dup(net, a, b, *alloc_head);
        *alloc_head += 4;
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Pair discovery + p-bit MIS conflict-free scheduling
 * ------------------------------------------------------------------------ */

typedef struct {
    uint32_t a;
    uint32_t b;
} pair_t;

static void find_pairs(const uint64_t *net, uint32_t n_agents,
                       pair_t *pairs, uint32_t *n_pairs) {
    *n_pairs = 0;
    for (uint32_t i = 0; i < n_agents; i++) {
        uint64_t w = net[i];
        if (WORD_DEAD(w)) continue;
        uint32_t j = WORD_PORT_ADDR(w, 0);
        if (j == i || j >= n_agents) continue;
        uint64_t wj = net[j];
        if (WORD_DEAD(wj)) continue;
        if (WORD_PORT_ADDR(wj, 0) != i) continue;
        if (i < j) {
            pairs[(*n_pairs)++].a = i;
            pairs[(*n_pairs) - 1].b = j;
        }
    }
}

/* Two pairs conflict if they share an agent.  Pick a conflict-free subset
 * using a deterministic p-bit (Boltzmann) anneal, mirroring the GPU MIS. */
static uint32_t pbit_mis_cpu(const pair_t *pairs, uint32_t n_pairs,
                             uint8_t *selected) {
    if (n_pairs == 0) return 0;
    bine_rng_t rng;
    rng.s = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)n_pairs * 0xBF58476D1CE4E5B9ULL);

    uint8_t *state = (uint8_t *)malloc(n_pairs);
    uint32_t *conflict = (uint32_t *)malloc(n_pairs * sizeof(uint32_t));
    if (!state || !conflict) {
        free(state);
        free(conflict);
        for (uint32_t i = 0; i < n_pairs && i < 1; i++) selected[i] = 1;
        return n_pairs < 1 ? 0 : 1;
    }

    /* Initialize conflict counts */
    for (uint32_t i = 0; i < n_pairs; i++) conflict[i] = 0;
    for (uint32_t i = 0; i < n_pairs; i++) {
        for (uint32_t j = i + 1; j < n_pairs; j++) {
            if (pairs[i].a == pairs[j].a || pairs[i].a == pairs[j].b ||
                pairs[i].b == pairs[j].a || pairs[i].b == pairs[j].b) {
                conflict[i]++;
                conflict[j]++;
            }
        }
    }

    /* Start with low-conflict pairs selected */
    for (uint32_t i = 0; i < n_pairs; i++) {
        selected[i] = (conflict[i] <= 2) ? 1 : 0;
        state[i] = selected[i];
    }

    /* Anneal: flip to reduce conflicts */
    for (uint32_t iter = 0; iter < 64; iter++) {
        double beta = 1.0 + (double)iter; /* increasing determinism */
        uint32_t changes = 0;
        for (uint32_t i = 0; i < n_pairs; i++) {
            uint32_t c = conflict[i];
            double p_keep = 1.0 / (1.0 + exp(-beta * (4.0 - (double)c)));
            state[i] = (rng_uniform(&rng) < p_keep) ? 1 : 0;
            if (state[i] != selected[i]) { selected[i] = state[i]; changes++; }
        }
        if (changes == 0) break;
    }

    /* Verify conflict-free; drop conflicting pairs deterministically */
    for (uint32_t i = 0; i < n_pairs; i++) {
        if (!selected[i]) continue;
        for (uint32_t j = 0; j < i; j++) {
            if (!selected[j]) continue;
            if (pairs[i].a == pairs[j].a || pairs[i].a == pairs[j].b ||
                pairs[i].b == pairs[j].a || pairs[i].b == pairs[j].b) {
                selected[i] = 0; /* keep lower index */
                break;
            }
        }
    }

    uint32_t n_sel = 0;
    for (uint32_t i = 0; i < n_pairs; i++) n_sel += selected[i] ? 1 : 0;
    free(state);
    free(conflict);
    return n_sel;
}

/* --------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */

int bine_icor_reduce_cpu(const uint64_t *net, uint32_t n_agents,
                         uint64_t **out_net, uint32_t *n_out,
                         uint32_t *rounds) {
    if (!net || !out_net || !n_out || n_agents == 0) return -1;

    /* Capacity: original + generous headroom for CON><DUP expansions. */
    uint32_t cap = n_agents * 4 + 1024;
    uint64_t *work = (uint64_t *)malloc(cap * sizeof(uint64_t));
    if (!work) return -1;
    memcpy(work, net, n_agents * sizeof(uint64_t));

    uint32_t alloc_head = n_agents;
    pair_t *pairs = (pair_t *)malloc(cap * sizeof(pair_t));
    uint8_t *sel = (uint8_t *)malloc(cap);
    if (!pairs || !sel) {
        free(pairs);
        free(sel);
        free(work);
        return -1;
    }

    uint32_t n_rounds = 0;
    const uint32_t max_rounds = 1000;
    while (n_rounds < max_rounds) {
        uint32_t n_pairs = 0;
        find_pairs(work, alloc_head, pairs, &n_pairs);
        if (n_pairs == 0) break;

        uint32_t n_mis = pbit_mis_cpu(pairs, n_pairs, sel);
        uint32_t applied = 0;
        for (uint32_t i = 0; i < n_pairs; i++) {
            if (!sel[i]) continue;
            if (apply_rule(work, &alloc_head, cap, pairs[i].a, pairs[i].b)) {
                applied++;
            }
        }
        n_rounds++;
        if (applied == 0 && n_rounds > 4) break; /* stuck: give up */
    }

    free(pairs);
    free(sel);

    /* Trim to actual allocation (slots beyond alloc_head are garbage). */
    uint64_t *result = (uint64_t *)malloc(alloc_head * sizeof(uint64_t));
    if (!result) {
        free(work);
        return -1;
    }
    memcpy(result, work, alloc_head * sizeof(uint64_t));
    free(work);

    *out_net = result;
    *n_out = alloc_head;
    if (rounds) *rounds = n_rounds;
    return 0;
}

/* Convenience: does agent i represent a live instruction after reduction?
 * Out-of-range (synthetic) agents are never instructions. */
int bine_icor_agent_live(const uint64_t *reduced, uint32_t n_reduced,
                         uint32_t i) {
    if (i >= n_reduced) return 0;
    uint64_t w = reduced[i];
    if (WORD_DEAD(w)) return 0;
    if (WORD_TYPE(w) == 2) return 0; /* ERA */
    return 1;
}
