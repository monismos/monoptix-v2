#include "bine.h"
#include <string.h>

static const uint8_t x86_nop_patterns[][15] = {
    {0x90},
    {0x0F, 0x1F, 0x00},
    {0x0F, 0x1F, 0x40, 0x00},
    {0x0F, 0x1F, 0x44, 0x00, 0x00},
    {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},
    {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},
    {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
};

static const uint8_t nop_pattern_lengths[] = {1, 3, 4, 5, 6, 7, 8, 9, 10};
static const int n_nop_patterns = 9;

uint64_t detect_nops(const uint8_t* text, uint64_t text_size, uint64_t* nop_offsets, uint64_t max_nops) {
    uint64_t found = 0;
    for (uint64_t i = 0; i < text_size && found < max_nops; ) {
        int matched = 0;
        for (int p = n_nop_patterns - 1; p >= 0; p--) {
            int len = nop_pattern_lengths[p];
            if (i + len <= text_size && memcmp(text + i, x86_nop_patterns[p], (size_t)len) == 0) {
                if (nop_offsets) nop_offsets[found] = i;
                found++;
                i += len;
                matched = 1;
                break;
            }
        }
        if (!matched) i++;
    }
    return found;
}

uint64_t detect_redundant_sequences(const uint8_t* data, uint64_t size,
                                    uint64_t min_length, uint64_t* offsets, uint64_t* lengths, uint64_t max_results) {
    uint64_t found = 0;
    for (uint64_t i = 0; i < size - min_length && found < max_results; i++) {
        for (uint64_t j = i + min_length; j < size - min_length; j++) {
            if (data[i] == data[j]) {
                uint64_t match_len = 0;
                while (i + match_len < size && j + match_len < size &&
                       data[i + match_len] == data[j + match_len] && match_len < 4096) {
                    match_len++;
                }
                if (match_len >= min_length) {
                    int already_found = 0;
                    for (uint64_t k = 0; k < found; k++) {
                        if (offsets[k] == i) { already_found = 1; break; }
                    }
                    if (!already_found) {
                        offsets[found] = i;
                        lengths[found] = match_len;
                        found++;
                    }
                    j += match_len;
                }
            }
        }
    }
    return found;
}
