// icor/rules.cuh — the 6 rules, inline device functions, pure bit operations.

#ifndef ICOR_RULES_CUH
#define ICOR_RULES_RULES_CUH

#include "bits.cuh"
#include <cstdint>

// ── RULE 1: CON >< CON — cross-annihilate ──────────────────────────────────
// a_left connects to b_right, a_right connects to b_left.
// Both agent words are SET_DEAD. Two rewires happen in the surrounding words.

__device__ __forceinline__
void rule_con_con(uint64_t* net, uint32_t a, uint32_t b) {
    uint64_t wa = net[a], wb = net[b];

    uint64_t a_left  = WORD_PORTREF(wa, 1);   // a's aux-left connection
    uint64_t a_right = WORD_PORTREF(wa, 2);   // a's aux-right connection
    uint64_t b_left  = WORD_PORTREF(wb, 1);
    uint64_t b_right = WORD_PORTREF(wb, 2);

    // Rewire: target of a_left now points to b_right's target
    // i.e., whoever was connected to a's left port now connects to whoever
    // was connected to b's right port.
    uint32_t al_a = WORD_PORT_ADDR(wa, 1);  uint8_t al_p = WORD_PORT_ID(wa, 1);
    uint32_t ar_a = WORD_PORT_ADDR(wa, 2);  uint8_t ar_p = WORD_PORT_ID(wa, 2);
    uint32_t bl_a = WORD_PORT_ADDR(wb, 1);  uint8_t bl_p = WORD_PORT_ID(wb, 1);
    uint32_t br_a = WORD_PORT_ADDR(wb, 2);  uint8_t br_p = WORD_PORT_ID(wb, 2);

    // Cross-connect: a_left ↔ b_right
    net[al_a] = SET_PORT(net[al_a], al_p, PORTREF(br_a, br_p));
    net[br_a] = SET_PORT(net[br_a], br_p, PORTREF(al_a, al_p));
    // Cross-connect: a_right ↔ b_left
    net[ar_a] = SET_PORT(net[ar_a], ar_p, PORTREF(bl_a, bl_p));
    net[bl_a] = SET_PORT(net[bl_a], bl_p, PORTREF(ar_a, ar_p));

    // Tombstone
    net[a] = SET_DEAD(wa);
    net[b] = SET_DEAD(wb);
}

// ── RULE 2: DUP >< DUP — straight-annihilate ───────────────────────────────
// a_left connects to b_left, a_right connects to b_right. Same as CON><CON
// but straight instead of crossed. Reuse the same function with a flag:

__device__ __forceinline__
void rule_dup_dup(uint64_t* net, uint32_t a, uint32_t b) {
    uint64_t wa = net[a], wb = net[b];
    uint32_t al_a = WORD_PORT_ADDR(wa,1); uint8_t al_p = WORD_PORT_ID(wa,1);
    uint32_t ar_a = WORD_PORT_ADDR(wa,2); uint8_t ar_p = WORD_PORT_ID(wa,2);
    uint32_t bl_a = WORD_PORT_ADDR(wb,1); uint8_t bl_p = WORD_PORT_ID(wb,1);
    uint32_t br_a = WORD_PORT_ADDR(wb,2); uint8_t br_p = WORD_PORT_ID(wb,2);

    // Straight-connect: a_left ↔ b_left
    net[al_a] = SET_PORT(net[al_a], al_p, PORTREF(bl_a, bl_p));
    net[bl_a] = SET_PORT(net[bl_a], bl_p, PORTREF(al_a, al_p));
    // Straight-connect: a_right ↔ b_right
    net[ar_a] = SET_PORT(net[ar_a], ar_p, PORTREF(br_a, br_p));
    net[br_a] = SET_PORT(net[br_a], br_p, PORTREF(ar_a, ar_p));

    net[a] = SET_DEAD(wa);
    net[b] = SET_DEAD(wb);
}

// ── RULE 3: ERA >< ERA — vanish ─────────────────────────────────────────────
// Both agents have no aux ports. Just tombstone both words.

__device__ __forceinline__
void rule_era_era(uint64_t* net, uint32_t a, uint32_t b) {
    net[a] = SET_DEAD(net[a]);
    net[b] = SET_DEAD(net[b]);
}

// ── RULES 5 & 6: X >< ERA — erase one agent ────────────────────────────────
// The non-ERA agent X has two aux ports. Each aux port gets a fresh ERA.
// Reuse the a and b slots (tombstone b if it was ERA, reuse a for first new ERA).
// Allocate one additional ERA slot.

__device__ __forceinline__
void rule_x_era(uint64_t* net, uint32_t x, uint32_t era_slot,
                uint32_t fresh_era_slot)
{
    uint64_t wx = net[x];
    uint32_t l_a = WORD_PORT_ADDR(wx, 1); uint8_t l_p = WORD_PORT_ID(wx, 1);
    uint32_t r_a = WORD_PORT_ADDR(wx, 2); uint8_t r_p = WORD_PORT_ID(wx, 2);

    // era_slot becomes ERA connected to left aux target
    uint64_t era_word_l = SET_TYPE(0ULL, 2);  // type=ERA=10b, all ports zero
    era_word_l = SET_PORT(era_word_l, 0, PORTREF(l_a, l_p));
    net[era_slot] = era_word_l;
    net[l_a] = SET_PORT(net[l_a], l_p, PORTREF(era_slot, 0));

    // fresh_era_slot becomes ERA connected to right aux target
    uint64_t era_word_r = SET_TYPE(0ULL, 2);
    era_word_r = SET_PORT(era_word_r, 0, PORTREF(r_a, r_p));
    net[fresh_era_slot] = era_word_r;
    net[r_a] = SET_PORT(net[r_a], r_p, PORTREF(fresh_era_slot, 0));

    // Tombstone original X
    net[x] = SET_DEAD(wx);
}

// ── RULE 4: CON >< DUP — commutation. Creates 4 new agents. ────────────────
// This is the only rule that expands the net.
// d1, d2, c1, c2 are freshly allocated contiguous slots: [base, base+3].
// All construction is pure bit assembly.

__device__ __forceinline__
void rule_con_dup(uint64_t* net, uint32_t con, uint32_t dup, uint32_t base) {
    uint64_t wc = net[con], wd = net[dup];
    uint8_t con_type = (uint8_t)WORD_TYPE(wc);   // should be CON=00
    uint8_t dup_type = (uint8_t)WORD_TYPE(wd);   // should be DUP=01

    uint32_t d1 = base, d2 = base+1, c1 = base+2, c2 = base+3;

    // External connections of original CON: c_left, c_right
    uint32_t cl_a = WORD_PORT_ADDR(wc,1); uint8_t cl_p = WORD_PORT_ID(wc,1);
    uint32_t cr_a = WORD_PORT_ADDR(wc,2); uint8_t cr_p = WORD_PORT_ID(wc,2);
    // External connections of original DUP: d_left, d_right
    uint32_t dl_a = WORD_PORT_ADDR(wd,1); uint8_t dl_p = WORD_PORT_ID(wd,1);
    uint32_t dr_a = WORD_PORT_ADDR(wd,2); uint8_t dr_p = WORD_PORT_ID(wd,2);

    // Build d1 word (type = dup_type):
    // principal → cl (external left of CON)
    // aux_left  → c1.aux_left  (new internal wire)
    // aux_right → c2.aux_left  (new internal wire)
    uint64_t wd1 = SET_TYPE(0ULL, dup_type);
    wd1 = SET_PORT(wd1, 0, PORTREF(cl_a, cl_p));
    wd1 = SET_PORT(wd1, 1, PORTREF(c1, 1));
    wd1 = SET_PORT(wd1, 2, PORTREF(c2, 1));
    net[d1] = wd1;

    // Build d2 word (type = dup_type):
    uint64_t wd2 = SET_TYPE(0ULL, dup_type);
    wd2 = SET_PORT(wd2, 0, PORTREF(cr_a, cr_p));
    wd2 = SET_PORT(wd2, 1, PORTREF(c1, 2));
    wd2 = SET_PORT(wd2, 2, PORTREF(c2, 2));
    net[d2] = wd2;

    // Build c1 word (type = con_type):
    uint64_t wc1 = SET_TYPE(0ULL, con_type);
    wc1 = SET_PORT(wc1, 0, PORTREF(dl_a, dl_p));
    wc1 = SET_PORT(wc1, 1, PORTREF(d1, 1));
    wc1 = SET_PORT(wc1, 2, PORTREF(d2, 1));
    net[c1] = wc1;

    // Build c2 word (type = con_type):
    uint64_t wc2 = SET_TYPE(0ULL, con_type);
    wc2 = SET_PORT(wc2, 0, PORTREF(dr_a, dr_p));
    wc2 = SET_PORT(wc2, 1, PORTREF(d1, 2));
    wc2 = SET_PORT(wc2, 2, PORTREF(d2, 2));
    net[c2] = wc2;

    // Update external neighbors to point to new agents
    net[cl_a] = SET_PORT(net[cl_a], cl_p, PORTREF(d1, 0));
    net[cr_a] = SET_PORT(net[cr_a], cr_p, PORTREF(d2, 0));
    net[dl_a] = SET_PORT(net[dl_a], dl_p, PORTREF(c1, 0));
    net[dr_a] = SET_PORT(net[dr_a], dr_p, PORTREF(c2, 0));

    // Tombstone originals
    net[con] = SET_DEAD(wc);
    net[dup] = SET_DEAD(wd);
}

#endif // ICOR_RULES_CUH
