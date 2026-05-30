#include "bine.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_RULES 65536
#define MAX_SEQ 1048576

typedef struct {
    uint16_t left;
    uint16_t right;
    uint32_t freq;
} RepairRule;

typedef struct {
    RepairRule rules[MAX_RULES];
    uint32_t n_rules;
    uint16_t* sequence;
    uint32_t seq_len;
} RepairGrammar;

static uint32_t pair_hash(uint16_t a, uint16_t b) {
    return (a * 65537u) ^ (b * 2654435761u);
}

uint64_t* repair_compress(const uint8_t* data, uint64_t size, uint32_t* n_rules_out, uint64_t* compressed_size_out) {
    uint64_t word_capacity = (size / 8) + 256;
    uint64_t* result = calloc(word_capacity, sizeof(uint64_t));
    if (!result) return NULL;
    result[0] = size;
    result[1] = 0;
    uint64_t result_pos = 2;
    *n_rules_out = 0;
    *compressed_size_out = 0;
    uint64_t seq_cap = size < MAX_SEQ ? size : MAX_SEQ;
    uint16_t* seq = malloc(seq_cap * sizeof(uint16_t));
    if (!seq) { free(result); return NULL; }
    for (uint64_t i = 0; i < size && i < seq_cap; i++) seq[i] = data[i];
    uint32_t seq_len = (uint32_t)(size < seq_cap ? size : seq_cap);
    RepairRule rules[MAX_RULES];
    uint32_t n_rules = 0;
    while (n_rules < MAX_RULES) {
        uint32_t pair_freq[65536] = {0};
        uint32_t pair_second[65536] = {0};
        for (uint32_t i = 0; i < seq_len - 1; i++) {
            uint32_t hash = pair_hash(seq[i], seq[i + 1]) & 0xFFFF;
            pair_freq[hash]++;
            pair_second[hash] = seq[i + 1];
        }
        uint32_t best_hash = 0;
        uint32_t best_freq = 0;
        for (uint32_t h = 0; h < 65536; h++) {
            if (pair_freq[h] > best_freq) {
                best_freq = pair_freq[h];
                best_hash = h;
            }
        }
        if (best_freq < 2) break;
        uint16_t new_sym = (uint16_t)(256 + n_rules);
        rules[n_rules].left = seq[0];
        rules[n_rules].right = pair_second[best_hash];
        uint16_t target_left = 0;
        for (uint32_t i = 0; i < seq_len - 1; i++) {
            uint32_t h = pair_hash(seq[i], seq[i + 1]) & 0xFFFF;
            if (h == best_hash) {
                target_left = seq[i];
                rules[n_rules].left = target_left;
                rules[n_rules].right = seq[i + 1];
                break;
            }
        }
        uint32_t write_pos = 0;
        for (uint32_t i = 0; i < seq_len; i++) {
            if (i < seq_len - 1 && seq[i] == rules[n_rules].left && seq[i + 1] == rules[n_rules].right) {
                seq[write_pos++] = new_sym;
                i++;
            } else {
                seq[write_pos++] = seq[i];
            }
        }
        seq_len = write_pos;
        n_rules++;
    }
    for (uint32_t i = 0; i < n_rules; i++) {
        if (result_pos + 1 >= word_capacity) {
            word_capacity *= 2;
            uint64_t* new_result = realloc(result, word_capacity * sizeof(uint64_t));
            if (!new_result) { free(result); free(seq); return NULL; }
            result = new_result;
        }
        result[result_pos++] = ((uint64_t)rules[i].left << 48) |
                               ((uint64_t)rules[i].right << 32) |
                               ((uint64_t)rules[i].freq);
    }
    uint64_t packed_words = (seq_len + 3) / 4;
    for (uint32_t i = 0; i < seq_len; i += 4) {
        if (result_pos >= word_capacity) {
            word_capacity *= 2;
            uint64_t* new_result = realloc(result, word_capacity * sizeof(uint64_t));
            if (!new_result) { free(result); free(seq); return NULL; }
            result = new_result;
        }
        uint64_t w = 0;
        for (int j = 0; j < 4 && i + j < seq_len; j++)
            w |= ((uint64_t)seq[i + j] << (16 * j));
        result[result_pos++] = w;
    }
    result[1] = n_rules;
    *n_rules_out = n_rules;
    *compressed_size_out = result_pos * sizeof(uint64_t);
    free(seq);
    return result;
}

uint8_t* repair_decompress(const uint64_t* rules_data, uint32_t n_rules, uint64_t* output_size) {
    (void)rules_data;
    (void)n_rules;
    (void)output_size;
    return NULL;
}
