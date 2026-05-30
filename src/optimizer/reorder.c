#include "bine.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    uint32_t block_id;
    uint64_t start;
    uint64_t end;
    uint64_t size;
    uint32_t freq;
    bool is_hot;
} ReorderBlock;

static int block_hot_cmp(const void* a, const void* b) {
    const ReorderBlock* ba = (const ReorderBlock*)a;
    const ReorderBlock* bb = (const ReorderBlock*)b;
    if (ba->is_hot != bb->is_hot) return bb->is_hot - ba->is_hot;
    if (ba->freq != bb->freq) return (int)(bb->freq - ba->freq);
    return (int)(ba->start - bb->start);
}

uint64_t bine_reorder_code(const uint8_t* text, uint64_t text_size,
                           const BasicBlock* blocks, uint32_t n_blocks,
                           const bool* hotness, uint8_t* out) {
    if (!text || !out || !blocks || n_blocks == 0) {
        if (text && out) memcpy(out, text, text_size);
        return text_size;
    }
    ReorderBlock* rb = calloc(n_blocks, sizeof(ReorderBlock));
    if (!rb) { memcpy(out, text, text_size); return text_size; }
    uint64_t total_size = 0;
    for (uint32_t i = 0; i < n_blocks; i++) {
        rb[i].block_id = i;
        rb[i].start = blocks[i].start;
        rb[i].end = blocks[i].end;
        rb[i].size = blocks[i].end - blocks[i].start;
        rb[i].freq = 1;
        rb[i].is_hot = hotness ? hotness[i] : false;
        total_size += rb[i].size;
    }
    if (total_size > text_size) total_size = text_size;
    qsort(rb, n_blocks, sizeof(ReorderBlock), block_hot_cmp);
    uint64_t write_pos = 0;
    for (uint32_t i = 0; i < n_blocks; i++) {
        uint64_t len = rb[i].size;
        if (rb[i].start + len > text_size) len = text_size - rb[i].start;
        if (write_pos + len > text_size) len = text_size - write_pos;
        if (len > 0) {
            memcpy(out + write_pos, text + rb[i].start, len);
            write_pos += len;
        }
    }
    if (write_pos < text_size)
        memset(out + write_pos, 0x90, text_size - write_pos);
    free(rb);
    return write_pos;
}
