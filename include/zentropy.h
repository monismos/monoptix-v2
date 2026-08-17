/*
 * zentropy.h - Z-entropy Numbers
 *
 * A Z-entropy number n = (b, e, z, P) is a quadruple:
 *   b:  sign bit (0/1)
 *   e:  exponent (int)
 *   z:  self-information = -log2 P(n)
 *   P:  empirical probability in training corpus
 *
 * Encoding a sequence (n_1, ..., n_k) costs:
 *   H = sum_i z(n_i) = -sum_i log2 P(n_i)
 * which is the Shannon entropy of the sequence.
 *
 * By Shannon's source-coding theorem, no encoding can do better
 * in expectation over the corpus distribution.  Thus the Z-entropy
 * encoding is asymptotically optimal.
 *
 * Practical implementation: Huffman coding with a frequency table
 * built from the corpus (ls, bash, cat, date, echo, true).
 */

#ifndef BINE_ZENTROPY_H
#define BINE_ZENTROPY_H

#include "bbf.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZENTROPY_MAX_SYMBOLS  512   /* BBF node types + aux data */
#define ZENTROPY_MAX_CODE     32   /* bits per codeword */
#define ZENTROPY_TABLE_MAGIC  0x5A454E54u  /* "ZENT" */

typedef struct {
    uint8_t  b;       /* sign */
    uint8_t  pad;
    uint16_t e;       /* exponent (signed via 2's complement) */
    uint32_t z_x1000; /* self-information * 1000 (fixed-point) */
    uint32_t count;   /* occurrences in corpus */
} zentropy_entry_t;

/* Frequency table: maps a 16-bit key (type<<8 | flags) to a Z-entropy entry. */
typedef struct {
    uint32_t          magic;
    zentropy_entry_t  table[ZENTROPY_MAX_SYMBOLS];
    uint32_t          total_count;
    uint32_t          num_symbols;
    /* Huffman tree (built lazily from frequency table) */
    struct {
        int16_t  left;
        int16_t  right;
        int16_t  parent;
        uint16_t symbol;
        uint32_t freq;
    }                  nodes[2 * ZENTROPY_MAX_SYMBOLS];
    int                root;
    /* Codebook: code[i] = bit pattern, len[i] = bit length */
    uint32_t           code[ZENTROPY_MAX_SYMBOLS];
    uint8_t            len [ZENTROPY_MAX_SYMBOLS];
} zentropy_table_t;

int  zentropy_table_init(zentropy_table_t *t);
int  zentropy_table_observe(zentropy_table_t *t, uint16_t key);
int  zentropy_table_finalize(zentropy_table_t *t);  /* build Huffman */
int  zentropy_table_save(const zentropy_table_t *t, const char *path);
int  zentropy_table_load(zentropy_table_t *t, const char *path);

/* Compute total bits needed to encode a sequence of keys. */
uint64_t zentropy_encode_cost(const zentropy_table_t *t,
                              const uint16_t *keys, size_t n);

/* Compute self-information for one key (fixed-point * 1000). */
uint32_t zentropy_self_info(const zentropy_table_t *t, uint16_t key);

/* Bit-level encode/decode of a sequence (writes to / reads from a bit buffer). */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;       /* bits used so far */
    uint64_t bits;      /* shift register */
    int      nbits;     /* bits in register */
} zentropy_bitbuf_t;

int  zentropy_bitbuf_init(zentropy_bitbuf_t *bb, size_t cap_bytes);
void zentropy_bitbuf_free(zentropy_bitbuf_t *bb);
int  zentropy_bitbuf_write(zentropy_bitbuf_t *bb, uint32_t code, int nbits);
int  zentropy_bitbuf_read (zentropy_bitbuf_t *bb, int nbits, uint32_t *out);
int  zentropy_bitbuf_flush(zentropy_bitbuf_t *bb);

int  zentropy_encode(const zentropy_table_t *t,
                     const uint16_t *keys, size_t n,
                     uint8_t **out, size_t *out_bits);
int  zentropy_decode(const zentropy_table_t *t,
                     const uint8_t *buf, size_t in_bits,
                     uint16_t *keys, size_t n_keys);

/* Convenience: build a default frequency table from the BBF node type
 * distribution (uniform baseline: each type gets 1 count). */
int zentropy_table_default(zentropy_table_t *t);

#ifdef __cplusplus
}
#endif

#endif /* BINE_ZENTROPY_H */
