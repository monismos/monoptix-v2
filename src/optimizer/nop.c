#include "bine.h"
#include <string.h>
#include <stdio.h>

uint64_t bine_remove_nops(uint8_t* text, uint64_t text_size, uint8_t* out) {
    uint64_t write_pos = 0;
    uint64_t nop_count = 0;
    uint64_t nop_offsets[65536];
    uint64_t n_nops = detect_nops(text, text_size, nop_offsets, 65536);
    uint64_t nop_idx = 0;
    for (uint64_t i = 0; i < text_size; i++) {
        if (nop_idx < n_nops && nop_offsets[nop_idx] == i) {
            uint64_t nop_start = nop_offsets[nop_idx];
            uint64_t nop_end = nop_start;
            while (nop_idx < n_nops && nop_offsets[nop_idx] == nop_end) {
                int len = 1;
                for (int p = 0; p < 9; p++) {
                    static const uint8_t lens[] = {1, 3, 4, 5, 6, 7, 8, 9, 10};
                    if (nop_end + lens[p] <= text_size &&
                        memcmp(text + nop_end, (const uint8_t[]){0x90,0x0F,0x1F,0x00,0x40,0x00,0x44,0x00,0x00,0x66}, lens[p]) == 0) {
                        len = lens[p]; break;
                    }
                }
                if (i + len <= text_size) {
                    nop_end += len;
                    i += len - 1;
                    nop_idx++;
                    nop_count++;
                } else break;
            }
        } else {
            out[write_pos++] = text[i];
        }
    }
    return write_pos;
}

typedef struct {
    uint64_t offset;
    uint64_t length;
    uint8_t bytes[15];
} NopSpan;

uint64_t detect_nop_spans(const uint8_t* text, uint64_t text_size, NopSpan* spans, uint64_t max_spans) {
    uint64_t n_spans = 0;
    uint8_t buf[15];
    for (uint64_t i = 0; i < text_size && n_spans < max_spans; ) {
        int matched = 0;
        for (int p = 8; p >= 0; p--) {
            static const uint8_t lens[] = {1, 3, 4, 5, 6, 7, 8, 9, 10};
            int len = lens[p];
            if (i + len <= text_size) {
                int ok = 1;
                for (int j = 0; j < len; j++) {
                    buf[j] = text[i + j];
                }
                if (len == 1 && buf[0] == 0x90) ok = 1;
                else if (len == 3 && buf[0] == 0x0F && buf[1] == 0x1F && buf[2] == 0x00) ok = 1;
                else if (len == 4 && buf[0] == 0x0F && buf[1] == 0x1F && buf[2] == 0x40 && buf[3] == 0x00) ok = 1;
                else if (len == 5 && buf[0] == 0x0F && buf[1] == 0x1F && buf[2] == 0x44 && buf[3] == 0x00 && buf[4] == 0x00) ok = 1;
                else if (len == 6 && buf[0] == 0x66 && buf[1] == 0x0F && buf[2] == 0x1F && buf[3] == 0x44 && buf[4] == 0x00 && buf[5] == 0x00) ok = 1;
                else if (len == 7 && buf[0] == 0x0F && buf[1] == 0x1F && buf[2] == 0x80 && buf[3] == 0x00 && buf[4] == 0x00 && buf[5] == 0x00 && buf[6] == 0x00) ok = 1;
                else ok = 0;
                if (ok) {
                    if (spans) {
                        spans[n_spans].offset = i;
                        spans[n_spans].length = len;
                        memcpy(spans[n_spans].bytes, buf, len);
                    }
                    n_spans++;
                    i += len;
                    matched = 1;
                    break;
                }
            }
        }
        if (!matched) i++;
    }
    return n_spans;
}
