#include "bine.h"
#include <string.h>
#include <stdlib.h>

typedef struct {
    uint64_t src_offset;
    uint64_t dst_offset;
    uint64_t length;
} MergeEntry;

uint64_t bine_merge_sequences(const uint8_t* text, uint64_t text_size, uint8_t* out, uint64_t* offset_map) {
    if (!text || !out) return 0;
    uint64_t head = 0;
    uint64_t tail = text_size;
    uint64_t write_pos = 0;
    uint64_t max_entries = 4096;
    MergeEntry* merges = calloc(max_entries, sizeof(MergeEntry));
    uint64_t n_merges = 0;
    for (uint64_t min_len = 64; min_len >= 4; min_len /= 2) {
        for (uint64_t i = 0; i < tail - min_len; i++) {
            for (uint64_t j = i + min_len; j < tail - min_len; j++) {
                if (text[i] == text[j]) {
                    uint64_t match = 0;
                    while (i + match < tail && j + match < tail &&
                           text[i + match] == text[j + match] && match < 4096) {
                        match++;
                    }
                    if (match >= min_len && n_merges < max_entries) {
                        int dup = 0;
                        for (uint64_t k = 0; k < n_merges; k++) {
                            if (merges[k].src_offset == i) { dup = 1; break; }
                        }
                        if (!dup) {
                            merges[n_merges].src_offset = i;
                            merges[n_merges].dst_offset = j;
                            merges[n_merges].length = match;
                            n_merges++;
                        }
                    }
                }
            }
        }
    }
    if (offset_map) {
        for (uint64_t i = 0; i < text_size; i++) offset_map[i] = i;
    }
    uint64_t read_pos = 0;
    while (read_pos < text_size) {
        int merged = 0;
        for (uint64_t m = 0; m < n_merges; m++) {
            if (merges[m].dst_offset == read_pos) {
                uint64_t src = merges[m].src_offset;
                uint64_t len = merges[m].length;
                memcpy(out + write_pos, text + src, len);
                if (offset_map) {
                    for (uint64_t k = 0; k < len; k++)
                        offset_map[read_pos + k] = src + k;
                }
                write_pos += len;
                read_pos += len;
                merged = 1;
                break;
            }
        }
        if (!merged) {
            out[write_pos++] = text[read_pos++];
        }
    }
    free(merges);
    return write_pos;
}
