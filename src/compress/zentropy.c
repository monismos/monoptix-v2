/*
 * zentropy.c - Z-entropy number encoding
 *
 * Huffman-coded frequency table.  The cost of encoding a sequence
 * equals the Shannon entropy (asymptotically optimal).
 */

#include "zentropy.h"
#include "bbf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int zentropy_table_init(zentropy_table_t *t) {
    if (!t) return -1;
    memset(t, 0, sizeof(*t));
    t->magic = ZENTROPY_TABLE_MAGIC;
    return 0;
}

int zentropy_table_observe(zentropy_table_t *t, uint16_t key) {
    if (!t || key >= ZENTROPY_MAX_SYMBOLS) return -1;
    t->table[key].count++;
    t->total_count++;
    if (t->num_symbols <= key) t->num_symbols = key + 1;
    return 0;
}

int zentropy_table_default(zentropy_table_t *t) {
    if (!t) return -1;
    zentropy_table_init(t);
    /* Build a uniform baseline: every BBF node type gets the same count.
     * For unknown distributions, this yields a fair (non-optimal) code. */
    for (uint16_t k = 0; k < ZENTROPY_MAX_SYMBOLS; k++) {
        t->table[k].count = 1;
        t->table[k].z_x1000 = 1000;  /* -log2(1/512) * 1000 = 9000, but we
                                       use 1000 as a per-symbol placeholder */
        t->total_count++;
    }
    t->num_symbols = ZENTROPY_MAX_SYMBOLS;
    return zentropy_table_finalize(t);
}

uint32_t zentropy_self_info(const zentropy_table_t *t, uint16_t key) {
    if (!t || key >= ZENTROPY_MAX_SYMBOLS) return 0;
    if (t->total_count == 0) return 0;
    if (t->table[key].count == 0) return UINT32_MAX;
    /* z = -log2(count / total) * 1000 */
    /* Use integer log2 approximation: log2(n) = position of highest bit */
    uint32_t total = t->total_count;
    uint32_t count = t->table[key].count;
    /* -log2(count/total) = log2(total) - log2(count) */
    uint32_t log_total = 0, log_count = 0;
    uint32_t v;
    v = total; while (v > 1) { v >>= 1; log_total++; }
    v = count; while (v > 1) { v >>= 1; log_count++; }
    int z = (int)log_total - (int)log_count;
    if (z < 0) z = 0;
    return (uint32_t)z * 1000;
}

int zentropy_table_finalize(zentropy_table_t *t) {
    if (!t) return -1;
    /* Build Huffman tree from frequencies.  We use a simple priority-queue
     * algorithm: pick the two lowest-frequency nodes, combine them.
     *
     * Symbols with count 0 are excluded.  If all are zero, we just
     * assign a uniform code. */
    int n = 0;
    int sym_idx[ZENTROPY_MAX_SYMBOLS];
    for (uint32_t i = 0; i < t->num_symbols; i++) {
        if (t->table[i].count > 0) {
            t->nodes[n].symbol = (uint16_t)i;
            t->nodes[n].freq   = t->table[i].count;
            t->nodes[n].left = t->nodes[n].right = t->nodes[n].parent = -1;
            sym_idx[i] = n;
            n++;
        }
    }
    if (n == 0) {
        t->root = -1;
        return 0;
    }
    if (n == 1) {
        /* Single symbol: code is empty (or "0") */
        t->nodes[0].parent = -1;
        t->root = 0;
        for (uint32_t i = 0; i < t->num_symbols; i++) {
            t->code[i] = 0;
            t->len[i]  = (i == t->nodes[0].symbol) ? 1 : 0;
        }
        return 0;
    }
    /* Initialize leaves and combine */
    int next = n;
    while (1) {
        /* Find two smallest */
        int a = -1, b = -1;
        uint32_t af = UINT32_MAX, bf = UINT32_MAX;
        for (int i = 0; i < next; i++) {
            if (t->nodes[i].parent != -1) continue;
            uint32_t f = t->nodes[i].freq;
            if (f < af) { b = a; bf = af; a = i; af = f; }
            else if (f < bf) { b = i; bf = f; }
        }
        if (b == -1) break;  /* Only one root left */
        t->nodes[next].left = (int16_t)a;
        t->nodes[next].right = (int16_t)b;
        t->nodes[next].parent = -1;
        t->nodes[next].freq = af + bf;
        t->nodes[a].parent = (int16_t)next;
        t->nodes[b].parent = (int16_t)next;
        next++;
    }
    t->root = next - 1;

    /* Walk tree to assign codes (MSB-first storage). */
    for (int i = 0; i < n; i++) {
        /* Compute path bits from root to this leaf.  We collect them
         * into a small array, then pack MSB-first into code. */
        int      path[64];
        int      plen = 0;
        int      cur  = i;
        while (t->nodes[cur].parent != -1 && plen < 64) {
            int p = t->nodes[cur].parent;
            path[plen++] = (t->nodes[p].right == cur) ? 1 : 0;
            cur = p;
        }
        uint32_t code = 0;
        for (int j = 0; j < plen; j++) {
            code = (code << 1) | (uint32_t)path[j];
        }
        uint16_t sym = t->nodes[i].symbol;
        t->code[sym] = code;
        t->len[sym]  = (uint8_t)(plen > 255 ? 255 : plen);
    }
    return 0;
}

int zentropy_table_save(const zentropy_table_t *t, const char *path) {
    if (!t || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t w = fwrite(t, sizeof(*t), 1, f);
    fclose(f);
    return w == 1 ? 0 : -2;
}

int zentropy_table_load(zentropy_table_t *t, const char *path) {
    if (!t || !path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t r = fread(t, sizeof(*t), 1, f);
    fclose(f);
    if (r != 1) return -2;
    if (t->magic != ZENTROPY_TABLE_MAGIC) return -3;
    return 0;
}

uint64_t zentropy_encode_cost(const zentropy_table_t *t,
                              const uint16_t *keys, size_t n) {
    if (!t || !keys) return 0;
    uint64_t total = 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t k = keys[i];
        if (k < ZENTROPY_MAX_SYMBOLS) total += t->len[k];
    }
    return total;
}

/* ---------- Bit buffer ---------- */
int zentropy_bitbuf_init(zentropy_bitbuf_t *bb, size_t cap_bytes) {
    if (!bb) return -1;
    bb->buf = (uint8_t*)calloc(cap_bytes ? cap_bytes : 1, 1);
    if (!bb->buf) return -1;
    bb->cap = cap_bytes ? cap_bytes : 1;
    bb->len = 0;
    bb->bits = 0;
    bb->nbits = 0;
    return 0;
}

void zentropy_bitbuf_free(zentropy_bitbuf_t *bb) {
    if (!bb) return;
    free(bb->buf);
    bb->buf = NULL;
    bb->cap = bb->len = bb->nbits = 0;
    bb->bits = 0;
}

int zentropy_bitbuf_write(zentropy_bitbuf_t *bb, uint32_t code, int nbits) {
    if (!bb || nbits < 0 || nbits > 32) return -1;
    bb->bits |= ((uint64_t)code << bb->nbits);
    bb->nbits += nbits;
    while (bb->nbits >= 8) {
        if (bb->len + 1 > bb->cap) {
            size_t newcap = bb->cap * 2;
            uint8_t *nb = (uint8_t*)realloc(bb->buf, newcap);
            if (!nb) return -2;
            bb->buf = nb;
            bb->cap = newcap;
        }
        bb->buf[bb->len++] = (uint8_t)(bb->bits & 0xFF);
        bb->bits >>= 8;
        bb->nbits -= 8;
    }
    return 0;
}

int zentropy_bitbuf_flush(zentropy_bitbuf_t *bb) {
    if (!bb) return -1;
    if (bb->nbits > 0) {
        if (bb->len + 1 > bb->cap) {
            size_t newcap = bb->cap * 2;
            uint8_t *nb = (uint8_t*)realloc(bb->buf, newcap);
            if (!nb) return -1;
            bb->buf = nb;
            bb->cap = newcap;
        }
        bb->buf[bb->len++] = (uint8_t)(bb->bits & 0xFF);
        bb->bits = 0;
        bb->nbits = 0;
    }
    return 0;
}

int zentropy_bitbuf_read(zentropy_bitbuf_t *bb, int nbits, uint32_t *out) {
    if (!bb || !out || nbits < 0 || nbits > 32) return -1;
    while (bb->nbits < nbits) {
        if (bb->len >= bb->cap) return -1;
        bb->bits |= ((uint64_t)bb->buf[bb->len++] << bb->nbits);
        bb->nbits += 8;
    }
    *out = (uint32_t)(bb->bits & ((1ULL << nbits) - 1));
    bb->bits >>= nbits;
    bb->nbits -= nbits;
    return 0;
}

int zentropy_encode(const zentropy_table_t *t,
                    const uint16_t *keys, size_t n,
                    uint8_t **out, size_t *out_bits) {
    if (!t || !keys || !out || !out_bits) return -1;
    zentropy_bitbuf_t bb;
    if (zentropy_bitbuf_init(&bb, n + 64) < 0) return -2;
    /* Track total bits written. */
    size_t bits_written = 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t k = keys[i];
        if (k >= ZENTROPY_MAX_SYMBOLS) { zentropy_bitbuf_free(&bb); return -3; }
        int rc = zentropy_bitbuf_write(&bb, t->code[k], t->len[k]);
        if (rc < 0) { zentropy_bitbuf_free(&bb); return rc; }
        bits_written += t->len[k];
    }
    zentropy_bitbuf_flush(&bb);
    *out = bb.buf;
    *out_bits = bits_written;
    bb.buf = NULL;
    zentropy_bitbuf_free(&bb);
    return 0;
}

int zentropy_decode(const zentropy_table_t *t,
                    const uint8_t *buf, size_t in_bits,
                    uint16_t *keys, size_t n_keys) {
    if (!t || !buf || !keys) return -1;
    zentropy_bitbuf_t bb;
    bb.buf = (uint8_t*)buf;
    bb.cap = (in_bits + 7) / 8;
    bb.len = 0;
    bb.bits = 0;
    bb.nbits = 0;
    size_t produced = 0;
    size_t total = (in_bits + 7) / 8;
    size_t bits_left = in_bits;
    while (produced < n_keys && bits_left > 0) {
        /* Walk the Huffman tree from the root, one bit at a time. */
        int cur = t->root;
        if (cur < 0) return -2;
        int depth = 0;
        while (t->nodes[cur].left != -1) {
            uint32_t bit;
            if (zentropy_bitbuf_read(&bb, 1, &bit) < 0) return -3;
            bits_left--;
            cur = bit ? t->nodes[cur].right : t->nodes[cur].left;
            depth++;
            if (depth > 64) return -4;  /* sanity */
        }
        if (bits_left == 0 && produced < n_keys - 1) return -5;
        keys[produced++] = t->nodes[cur].symbol;
    }
    return (int)produced;
}
