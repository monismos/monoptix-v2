#include "bine.h"
#include "bits.cuh"
#include <cuda_runtime.h>
#include <stdint.h>

// k_find_pairs: find all active pairs (main-port-to-main-port connections)
__global__ void k_find_pairs(uint64_t* net, uint32_t n_agents,
                             uint64_t* out_pairs, uint32_t* out_count) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n_agents) return;
    uint64_t w = net[i];
    if (WORD_DEAD(w)) return;
    uint32_t conn = (uint32_t)WORD_PORT_ADDR(w, 0);
    if (conn >= n_agents) return;
    uint64_t wc = net[conn];
    if (WORD_DEAD(wc)) return;
    if ((uint32_t)WORD_PORT_ADDR(wc, 0) == i) {
        uint32_t pair_idx = atomicAdd(out_count, 1);
        out_pairs[pair_idx] = ((uint64_t)i << 32) | (uint64_t)conn;
    }
}

// k_apply_rules: apply interaction rules on given pairs
__global__ void k_apply_rules(uint64_t* net, uint64_t* pairs, uint32_t pair_count,
                              uint32_t* alloc_head, uint64_t* out_pairs, uint32_t* out_count) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= pair_count) return;
    uint64_t packed = pairs[i];
    uint32_t a = (uint32_t)(packed >> 32);
    uint32_t b = (uint32_t)packed;
    uint64_t wa = net[a], wb = net[b];
    if (WORD_DEAD(wa) || WORD_DEAD(wb)) return;
    uint32_t ta = WORD_TYPE(wa), tb = WORD_TYPE(wb);
    uint32_t al = WORD_PORT_ADDR(wa, 1), ar = WORD_PORT_ADDR(wa, 2);
    uint32_t bl = WORD_PORT_ADDR(wb, 1), br = WORD_PORT_ADDR(wb, 2);
    uint8_t ap = WORD_PORT_ID(wa, 1), arp = WORD_PORT_ID(wa, 2);
    uint8_t bp = WORD_PORT_ID(wb, 1), brp = WORD_PORT_ID(wb, 2);
    if (ta == 0 && tb == 0) {
        net[al] = SET_PORT(net[al], ap, PORTREF(br, brp));
        net[br] = SET_PORT(net[br], brp, PORTREF(al, ap));
        net[ar] = SET_PORT(net[ar], arp, PORTREF(bl, bp));
        net[bl] = SET_PORT(net[bl], bp, PORTREF(ar, arp));
        net[a] = SET_DEAD(wa); net[b] = SET_DEAD(wb);
    } else if (ta == 1 && tb == 1) {
        net[al] = SET_PORT(net[al], ap, PORTREF(bl, bp));
        net[bl] = SET_PORT(net[bl], bp, PORTREF(al, ap));
        net[ar] = SET_PORT(net[ar], arp, PORTREF(br, brp));
        net[br] = SET_PORT(net[br], brp, PORTREF(ar, arp));
        net[a] = SET_DEAD(wa); net[b] = SET_DEAD(wb);
    } else if (ta == 2 && tb == 2) {
        net[a] = SET_DEAD(wa); net[b] = SET_DEAD(wb);
    } else if ((ta == 0 && tb == 1) || (ta == 1 && tb == 0)) {
        uint32_t con = (ta == 0) ? a : b;
        uint32_t dup = (ta == 1) ? a : b;
        uint64_t wcon = net[con], wdup = net[dup];
        uint32_t cl = WORD_PORT_ADDR(wcon, 1), cr = WORD_PORT_ADDR(wcon, 2);
        uint32_t dl = WORD_PORT_ADDR(wdup, 1), dr = WORD_PORT_ADDR(wdup, 2);
        uint8_t cp = WORD_PORT_ID(wcon, 1), crp = WORD_PORT_ID(wcon, 2);
        uint8_t dp = WORD_PORT_ID(wdup, 1), drp = WORD_PORT_ID(wdup, 2);
        uint32_t new_a = atomicAdd(alloc_head, 1);
        uint32_t new_b = atomicAdd(alloc_head, 1);
        if (new_a < 1048576 && new_b < 1048576) {
            net[new_a] = ((uint64_t)0 << 62) | PORTREF(con, 1) | (PORTREF(dl, dp) << 20) | (PORTREF(dr, drp) << 40);
            net[new_b] = ((uint64_t)0 << 62) | PORTREF(cl, cp) | (PORTREF(dl, dp) << 20) | (PORTREF(dr, drp) << 40);
            net[cl] = SET_PORT(net[cl], cp, PORTREF(new_b, 0));
            net[cr] = SET_PORT(net[cr], crp, PORTREF(new_a, 0));
            net[dl] = SET_PORT(net[dl], dp, PORTREF(new_a, 1));
            net[dr] = SET_PORT(net[dr], drp, PORTREF(new_a, 2));
        }
        net[con] = SET_DEAD(wcon); net[dup] = SET_DEAD(wdup);
    } else if ((ta == 0 && tb == 2) || (ta == 2 && tb == 0)) {
        uint32_t con = (ta == 0) ? a : b;
        uint32_t era = (ta == 2) ? a : b;
        net[era] = SET_DEAD(net[era]);
        uint32_t cl = WORD_PORT_ADDR(net[con], 1);
        uint32_t cr = WORD_PORT_ADDR(net[con], 2);
        uint8_t cp1 = WORD_PORT_ID(net[con], 1);
        uint8_t cp2 = WORD_PORT_ID(net[con], 2);
        net[cl] = SET_PORT(net[cl], cp1, PORTREF(era, 0));
        net[cr] = SET_PORT(net[cr], cp2, PORTREF(era, 0));
        uint32_t new_e = atomicAdd(alloc_head, 1);
        if (new_e < 1048576) net[new_e] = (uint64_t)2 << 62;
        net[con] = SET_DEAD(net[con]);
    } else if ((ta == 1 && tb == 2) || (ta == 2 && tb == 1)) {
        uint32_t dup = (ta == 1) ? a : b;
        uint32_t era = (ta == 2) ? a : b;
        net[era] = SET_DEAD(net[era]);
        net[dup] = SET_DEAD(net[dup]);
    }
}

// k_mark_live: mark non-dead agents
__global__ void k_mark_live(uint64_t* net, uint32_t n, uint32_t* flags) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    flags[i] = WORD_DEAD(net[i]) ? 0 : 1;
}

// k_scatter: compact live agents
__global__ void k_scatter(uint64_t* net, uint32_t* prefix, uint32_t* flags,
                          uint64_t* out_net, uint32_t* remap, uint32_t n) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    if (flags[i]) {
        uint32_t dst = prefix[i];
        out_net[dst] = net[i];
        remap[i] = dst;
    } else {
        remap[i] = UINT32_MAX;
    }
}

// k_remap_ports: update port references after compaction
__global__ void k_remap_ports(uint64_t* net, uint32_t* remap, uint32_t n) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    uint64_t w = net[i];
    for (int p = 0; p < 3; p++) {
        uint32_t addr = WORD_PORT_ADDR(w, p);
        if (addr < UINT32_MAX && remap[addr] != UINT32_MAX) {
            w = SET_PORT(w, p, PORTREF(remap[addr], WORD_PORT_ID(w, p)));
        }
    }
    net[i] = w;
}

// k_compute_hashes: compute hash for hash-consing
__global__ void k_compute_hashes(uint64_t* net, uint64_t* hashes, uint32_t* frontier,
                                 uint32_t frontier_n, uint32_t* next_frontier, uint32_t* next_count) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= frontier_n) return;
    uint32_t idx = frontier[i];
    if (idx >= 1048576) return;
    uint64_t w = net[idx];
    if (WORD_DEAD(w)) return;
    uint64_t h = w;
    for (int p = 0; p < 3; p++) {
        uint32_t conn = WORD_PORT_ADDR(w, p);
        if (conn < 1048576) h ^= hashes[conn] * 0x9E3779B97F4A7C15ULL;
    }
    hashes[idx] = h;
    uint32_t n = atomicAdd(next_count, 1);
    if (n < 1048576) next_frontier[n] = idx;
}
