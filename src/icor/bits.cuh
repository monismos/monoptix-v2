// icor/bits.cuh — ALL operations on the raw bit string live here.
// No other file is allowed to touch agent words except through these macros.

#ifndef ICOR_BITS_CUH
#define ICOR_BITS_CUH

#include <cstdint>

// Bit layout of one agent word (64 bits total):
//
//  63    62  61        44  43        26  25         8   7        2   1    0
// ┌──────┬─────────────┬─────────────┬─────────────┬───────────┬────┬────┐
// │ TYPE │  PORT_0     │  PORT_1     │  PORT_2     │  (spare)  │ AP │ DX │
// │ 2 b  │  18b addr   │  18b addr   │  18b addr   │   6 bits  │    │    │
// │      │  + 2b port  │  + 2b port  │  + 2b port  │           │    │    │
// └──────┴─────────────┴─────────────┴─────────────┴───────────┴────┴────┘
//
// TYPE  [63:62]  Agent type: 00=CON  01=DUP  10=ERA  11=VAR
// PORT_0[61:42]  Principal port connection: 18-bit agent address + 2-bit port id
// PORT_1[41:22]  Aux-left port connection:  18-bit agent address + 2-bit port id
// PORT_2[21: 2]  Aux-right port connection: 18-bit agent address + 2-bit port id
// AP    [    1]  Active Pair flag: 1 = this agent is the LESSER id in an active pair
// DX    [    0]  Dead flag: 1 = this agent has been consumed by a rule

// Extract type field (bits 63:62)
#define WORD_TYPE(w)          (((w) >> 62) & 0x3ULL)

// Extract port address from port slot p (0=principal, 1=aux_left, 2=aux_right)
#define WORD_PORT_ADDR(w, p)  (((w) >> (42 - (p)*20)) & 0x3FFFFULL)

// Extract port id from port slot p
#define WORD_PORT_ID(w, p)    (((w) >> (40 - (p)*20)) & 0x3ULL)

// Extract full portref (addr + port_id) from port slot p
#define WORD_PORTREF(w, p)    (((w) >> (42 - (p)*20)) & 0xFFFFFULL)

// Extract Active Pair flag (bit 1)
#define WORD_AP(w)            (((w) >>  1) & 0x1ULL)

// Extract Dead flag (bit 0)
#define WORD_DEAD(w)          (( w)        & 0x1ULL)

// Construct a portref (20 bits: 18 addr + 2 port_id)
#define PORTREF(addr, pid)    (((uint64_t)(addr) << 2) | ((uint64_t)(pid) & 0x3))

// Write a portref into port slot p of word w (returns new word, w unchanged)
#define SET_PORT(w, p, ref) \
    (((w) & ~(0xFFFFFULL << (42 - (p)*20))) | ((uint64_t)(ref) << (42 - (p)*20)))

// Set type field
#define SET_TYPE(w, t)  (((w) & ~(0x3ULL << 62)) | ((uint64_t)(t) << 62))

// Mark dead (set bit 0)
#define SET_DEAD(w)     ((w) | 0x1ULL)

// Mark as lesser-id partner of an active pair (set bit 1)
#define SET_AP(w)       ((w) | 0x2ULL)

// Clear Active Pair flag (clear bit 1)
#define CLEAR_AP(w)     ((w) & ~0x2ULL)

// Clear both AP and DEAD flags
#define CLEAR_FLAGS(w)  ((w) & ~0x3ULL)

#endif // ICOR_BITS_CUH
