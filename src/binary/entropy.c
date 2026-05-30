#include "bine.h"
#include <math.h>
#include <string.h>

static float shannon_entropy_byte(const uint8_t* data, uint64_t size, int byte_offset) {
    if (size == 0) return 0.0f;
    uint64_t count = 0;
    uint64_t zeros = 0;
    for (uint64_t i = byte_offset; i < size; i += 16) {
        count++;
        if ((data[i >> 3] >> (i & 7)) & 1) zeros++;
    }
    if (count == 0) return 0.0f;
    float p = (float)zeros / (float)count;
    if (p == 0.0f || p == 1.0f) return 0.0f;
    return -p * log2f(p) - (1.0f - p) * log2f(1.0f - p);
}

void entropy_scan(const uint8_t* data, uint64_t size, float* per_byte_entropy, float* total_entropy) {
    double sum = 0.0;
    uint64_t n_bytes = size < 128 ? size : 128;
    for (uint64_t i = 0; i < n_bytes && i < size; i++) {
        float e = shannon_entropy_byte(data, size, (int)i);
        if (per_byte_entropy) per_byte_entropy[i] = e;
        sum += e;
    }
    if (total_entropy) *total_entropy = (float)(sum / (double)(n_bytes > 0 ? n_bytes : 1));
}

void derive_masks_128(const uint8_t* data, uint64_t size,
                      __uint128_t* M_R, __uint128_t* M_W, __uint128_t* M_K) {
    __uint128_t read_mask = 0, write_mask = 0, kill_mask = 0;
    if (size < 16) { *M_R = 0; *M_W = 0; *M_K = 0; return; }
    uint64_t n_words = size / 16;
    uint64_t ones[128] = {0};
    for (uint64_t w = 0; w < n_words; w++) {
        for (int b = 0; b < 128; b++) {
            int byte_idx = (w * 16) + (b / 8);
            int bit_idx = b % 8;
            if (data[byte_idx] & (1 << bit_idx)) ones[b]++;
        }
    }
    for (int b = 0; b < 128; b++) {
        uint64_t zeros = n_words - ones[b];
        if (ones[b] > 0 && zeros > 0) {
            if (ones[b] <= zeros) read_mask |= ((__uint128_t)1 << b);
            if (ones[b] >= zeros) write_mask |= ((__uint128_t)1 << b);
        }
    }
    if (read_mask == 0) read_mask = ~(__uint128_t)0;
    if (write_mask == 0) write_mask = ~(__uint128_t)0;
    for (uint64_t w = 1; w < n_words; w++) {
        for (int b = 0; b < 128; b++) {
            int byte_idx_a = ((w - 1) * 16) + (b / 8);
            int bit_idx = b % 8;
            int byte_idx_b = (w * 16) + (b / 8);
            uint8_t bit_a = (data[byte_idx_a] >> bit_idx) & 1;
            uint8_t bit_b = (data[byte_idx_b] >> bit_idx) & 1;
            if (bit_a != bit_b) kill_mask |= ((__uint128_t)1 << b);
        }
    }
    *M_R = read_mask; *M_W = write_mask; *M_K = kill_mask;
}
