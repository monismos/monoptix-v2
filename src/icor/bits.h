// icor/bits.h — C-compatible host mirror of the 64-bit agent word layout.
//
// A net is an array of 64-bit words, one per agent.  The word layout is the
// legacy AgentPacked bitfield projection (see bine_internal.h) as produced
// by optimizer/net_build.c:
//
//   bit 63      : ESSENTIAL — agent may never be eliminated (set by net_build
//                 for CALL/RET/branch instructions)
//   bits 52-62  : reserved (port_3 / flags from the struct do not survive the
//                 uint64 cast; must be zero in a canonical word)
//   bits 36-51  : PORT2 (port_2 field)
//   bits 20-35  : PORT1 (port_1 field)
//   bits  4-19  : PORT0 (port_0 field)
//   bits  0-3   : TYPE  (AGENT_CON=0, AGENT_DUP=1, AGENT_ERA=2, AGENT_SIFT=3,
//                 AGENT_ATOMIC=4)
//
// Port fields hold agent indices (or register numbers / immediates in the
// ELF pipeline's DFG encoding; see net_build.c).  `PORTREF` packs an
// (address, port-id) pair into a 16-bit field for the ICOR rules.

#ifndef ICOR_BITS_H
#define ICOR_BITS_H

#include <stdint.h>

#define WORD_TYPE_MASK    0xFULL
#define WORD_TYPE_SHIFT   0
#define WORD_PORT0_MASK   0xFFFFULL
#define WORD_PORT0_SHIFT  4
#define WORD_PORT1_MASK   0xFFFFULL
#define WORD_PORT1_SHIFT  20
#define WORD_PORT2_MASK   0xFFFFULL
#define WORD_PORT2_SHIFT  36
#define WORD_ESSENTIAL_BIT 63ULL

#define WORD_TYPE(w)      (((w) >> WORD_TYPE_SHIFT) & WORD_TYPE_MASK)
#define WORD_PORT0(w)     (((w) >> WORD_PORT0_SHIFT) & WORD_PORT0_MASK)
#define WORD_PORT1(w)     (((w) >> WORD_PORT1_SHIFT) & WORD_PORT1_MASK)
#define WORD_PORT2(w)     (((w) >> WORD_PORT2_SHIFT) & WORD_PORT2_MASK)
#define WORD_ESSENTIAL(w) (((w) >> WORD_ESSENTIAL_BIT) & 0x1ULL)

#define SET_TYPE(w, t)     (((w) & ~(WORD_TYPE_MASK << WORD_TYPE_SHIFT)) | \
                            (((uint64_t)(t) & WORD_TYPE_MASK) << WORD_TYPE_SHIFT))
#define SET_PORT(w, p, v)  ((p) == 0 ? (((w) & ~(WORD_PORT0_MASK << WORD_PORT0_SHIFT)) | \
                                         (((uint64_t)(v) & WORD_PORT0_MASK) << WORD_PORT0_SHIFT)) \
                                      : ((p) == 1 ? (((w) & ~(WORD_PORT1_MASK << WORD_PORT1_SHIFT)) | \
                                                       (((uint64_t)(v) & WORD_PORT1_MASK) << WORD_PORT1_SHIFT)) \
                                                  : (((w) & ~(WORD_PORT2_MASK << WORD_PORT2_SHIFT)) | \
                                                     (((uint64_t)(v) & WORD_PORT2_MASK) << WORD_PORT2_SHIFT))))
#define SET_ESSENTIAL(w)   ((w) | (1ULL << WORD_ESSENTIAL_BIT))

#define WORD_DEAD(w)       (WORD_TYPE(w) == 2) /* ERA agents are dead */
#define SET_DEAD(w)        SET_TYPE(w, 2)      /* tombstone as ERA */

#define WORD_PORT(w, p)    ((p) == 0 ? WORD_PORT0(w) \
                            : (p) == 1 ? WORD_PORT1(w) : WORD_PORT2(w))
/* Legacy name used by the GPU-ICOR headers (same raw 16-bit field). */
#define WORD_PORTREF(w, p) WORD_PORT(w, p)
/* Legacy "active pair" probe bit (reserved bit 62): nothing sets it, so
 * kernels guarded by WORD_AP are inert — kept for API compatibility. */
#define WORD_AP(w)         (((w) >> 62) & 0x1ULL)
#define WORD_PORT_ADDR(v, p) (((WORD_PORT(v, p)) >> 4) & 0xFFF)
#define WORD_PORT_ID(v, p)   ((uint8_t)((WORD_PORT(v, p)) & 0xF))
#define PORTREF(a, p)      ((uint64_t)((((uint64_t)(a) & 0xFFF) << 4) | \
                                        ((uint64_t)(p) & 0xF)))

#endif /* ICOR_BITS_H */
