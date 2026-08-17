/*
 * strand/strand_p.c — Host-side STRAND-P probabilistic scheduler.
 *
 * Port of the p-bit Ising anneal from schedule.cu for CPU-only builds and
 * for the slot-stable ELF pipeline.  Produces a *schedule order* (a
 * permutation of instruction indices) rather than reordering a net, so the
 * serializer can apply only provably-safe same-length adjacent swaps.
 *
 * Deterministic: same seed and temperature schedule on every platform.
 */

#include "bine.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t s;
} strand_p_rng_t;

static uint64_t rng_next(strand_p_rng_t *r) {
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static double rng_uniform(strand_p_rng_t *r) {
    return (double)(rng_next(r) >> 11) * (1.0 / 9007199254740992.0);
}

/*
 * Anneal an Ising spin per instruction.  Couplings follow schedule.cu:
 *   J[i][j] = +1.0  when RAW/WAR/WAW conflict (antiferromagnetic)
 *   J[i][j] = -0.5  otherwise (ferromagnetic, encourages parallelism)
 * Final order = descending time-averaged spin.
 */
static int strand_p_anneal(uint32_t n,
                           const uint8_t *masks_r,
                           const uint8_t *masks_w,
                           const uint8_t *masks_k,
                           float *h_time_avg) {
    if (n == 0) return 0;

    float *couplings = (float *)calloc((size_t)n * n, sizeof(float));
    float *spins = (float *)malloc(n * sizeof(float));
    float *local_field = (float *)malloc(n * sizeof(float));
    float *time_avg = (float *)calloc(n, sizeof(float));
    if (!couplings || !spins || !local_field || !time_avg) {
        free(couplings);
        free(spins);
        free(local_field);
        free(time_avg);
        return -1;
    }

    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = i + 1; j < n; j++) {
            uint8_t raw = masks_r[i] & masks_w[j];
            uint8_t war = masks_w[i] & masks_r[j];
            uint8_t waw = masks_w[i] & masks_w[j];
            float j_ij = (raw || war || waw) ? 1.0f : -0.5f;
            couplings[i * n + j] = j_ij;
            couplings[j * n + i] = j_ij;
        }
    }

    for (uint32_t i = 0; i < n; i++) {
        spins[i] = (rng_next(&(strand_p_rng_t){i * 7919u + 1}) & 1) ? 1.0f : -1.0f;
    }

    const float temp_start = 2.0f;
    const float temp_end = 0.01f;
    const int max_iter = 1000;
    float best_energy = INFINITY;
    int stall = 0;

    for (int iter = 0; iter < max_iter; iter++) {
        /* Local field */
        for (uint32_t i = 0; i < n; i++) {
            float h = 0.0f;
            for (uint32_t j = 0; j < n; j++) h += couplings[i * n + j] * spins[j];
            local_field[i] = h;
        }

        /* Temperature schedule (geometric, same curve as schedule.cu) */
        float temp = temp_start *
                     powf(temp_end / temp_start, (float)iter / (float)max_iter);

        strand_p_rng_t rng;
        rng.s = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)iter * 0xBF58476D1CE4E5B9ULL) ^ (uint64_t)n;

        /* Boltzmann p-bit update */
        for (uint32_t i = 0; i < n; i++) {
            float h = local_field[i];
            float s = spins[i];
            float delta_e = -2.0f * s * h;
            if (delta_e < 0.0f) {
                spins[i] = -s;
            } else {
                float p = expf(-delta_e / (temp > 1e-6f ? temp : 1e-6f));
                if (rng_uniform(&rng) < p) spins[i] = -s;
            }
        }

        /* Energy */
        float energy = 0.0f;
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t j = 0; j < n; j++)
                energy += 0.5f * couplings[i * n + j] * spins[i] * spins[j];
        if (energy < best_energy) {
            best_energy = energy;
            stall = 0;
        } else if (++stall > 200) {
            break;
        }

        /* TApSA: exponential moving average of spins */
        const float alpha = 0.1f;
        for (uint32_t i = 0; i < n; i++) {
            time_avg[i] = alpha * spins[i] + (1.0f - alpha) * time_avg[i];
        }
    }

    for (uint32_t i = 0; i < n; i++) h_time_avg[i] = time_avg[i];

    free(couplings);
    free(spins);
    free(local_field);
    free(time_avg);
    return 0;
}

/*
 * Produce a schedule order (permutation of 0..n-1) sorted by descending
 * time-averaged spin.  Returns 0 on success.
 */
int bine_strand_p_schedule_cpu(uint32_t n_instructions,
                                const uint8_t *masks_r,
                                const uint8_t *masks_w,
                                const uint8_t *masks_k,
                                uint32_t *schedule_order) {
    if (!schedule_order || (n_instructions > 0 &&
                            (!masks_r || !masks_w || !masks_k))) {
        return -1;
    }
    if (n_instructions == 0) return 0;

    float *time_avg = (float *)malloc(n_instructions * sizeof(float));
    if (!time_avg) return -1;
    if (strand_p_anneal(n_instructions, masks_r, masks_w, masks_k, time_avg) != 0) {
        free(time_avg);
        return -1;
    }

    for (uint32_t i = 0; i < n_instructions; i++) schedule_order[i] = i;

    /* Stable insertion sort by descending spin */
    for (uint32_t i = 1; i < n_instructions; i++) {
        uint32_t j = i;
        while (j > 0 && time_avg[schedule_order[j]] > time_avg[schedule_order[j - 1]]) {
            uint32_t t = schedule_order[j];
            schedule_order[j] = schedule_order[j - 1];
            schedule_order[j - 1] = t;
            j--;
        }
    }

    free(time_avg);
    return 0;
}

/*
 * C-callable fallback with the same signature as the GPU wrapper in
 * schedule.cu.  NOTE: reorders the net (used by the .bbf/cubin workflows
 * where net index is not tied to ELF layout).  The ELF pipeline must NOT
 * use this; it consumes the schedule order directly.
 */
int bine_strand_p_optimize_wrapper(const uint64_t *net, uint32_t n_agents,
                                   const uint8_t *masks_r,
                                   const uint8_t *masks_w,
                                   const uint8_t *masks_k,
                                   uint64_t **scheduled_net,
                                   uint32_t *n_scheduled) {
    if (!net || !scheduled_net || !n_scheduled || n_agents == 0) return -1;

    uint32_t *order = (uint32_t *)malloc(n_agents * sizeof(uint32_t));
    if (!order) return -1;
    if (bine_strand_p_schedule_cpu(n_agents, masks_r, masks_w, masks_k, order) != 0) {
        free(order);
        return -1;
    }

    uint64_t *scheduled = (uint64_t *)malloc(n_agents * sizeof(uint64_t));
    if (!scheduled) {
        free(order);
        return -1;
    }
    for (uint32_t i = 0; i < n_agents; i++) scheduled[i] = net[order[i]];

    free(order);
    *scheduled_net = scheduled;
    *n_scheduled = n_agents;
    return 0;
}
