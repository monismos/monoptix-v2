// bine/apex/apex.c — APEX: Adaptive Pair Encoding with eXtensibility
// Implementation combining tANS entropy coding with GIE-ICOR grammar

#include "apex.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define APEX_FRAME_MAGIC "APX1"
#define APEX_FRAME_HEADER_SIZE 12

static void store_u64_le(uint8_t* dst, uint64_t value) {
    for (uint32_t i = 0; i < 8; i++) {
        dst[i] = (uint8_t)((value >> (i * 8)) & 0xFFu);
    }
}

static uint64_t load_u64_le(const uint8_t* src) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8; i++) {
        value |= ((uint64_t)src[i]) << (i * 8);
    }
    return value;
}

// Re-Pair algorithm: replace most frequent pair with new symbol
static void build_repair_grammar(APEXState* state) {
    if (!state->input || state->input_size < 2) return;
    
    // Working copy of input as symbol sequence
    uint32_t* symbols = (uint32_t*)malloc(state->input_size * sizeof(uint32_t));
    for (size_t i = 0; i < state->input_size; i++) {
        symbols[i] = state->input[i];  // Initially, each byte is a terminal
    }
    size_t n_symbols = state->input_size;
    
    // Allocate grammar rules (max possible is half the input size)
    uint32_t max_rules = (uint32_t)(state->input_size / 2) + 256;
    state->grammar.rules = (GrammarRule*)calloc(max_rules, sizeof(GrammarRule));
    state->grammar.n_terminals = 256;  // Byte values 0-255
    state->max_symbol = 255;  // Current highest symbol ID
    
    printf("[APEX] Building Re-Pair grammar from %zu symbols...\n", n_symbols);
    
    // Iteratively replace most frequent pair
    uint32_t n_iterations = 0;
    const uint32_t max_iterations = 1000;  // Prevent infinite loops
    
    while (n_symbols > 1 && n_iterations < max_iterations) {
        uint32_t dim = state->max_symbol + 1;
        size_t pair_cells = (size_t)dim * (size_t)dim;
        uint32_t* pf = (uint32_t*)realloc(state->pair_freq, pair_cells * sizeof(uint32_t));
        if (!pf) break;
        state->pair_freq = pf;
        memset(state->pair_freq, 0, pair_cells * sizeof(uint32_t));

        // Count all adjacent pairs
        
        for (size_t i = 0; i < n_symbols - 1; i++) {
            uint32_t a = symbols[i];
            uint32_t b = symbols[i + 1];
            if (a <= state->max_symbol && b <= state->max_symbol) {
                state->pair_freq[a * (state->max_symbol + 1) + b]++;
            }
        }
        
        // Find most frequent pair
        uint32_t max_freq = 0;
        uint32_t best_a = 0, best_b = 0;
        uint32_t table_size = (state->max_symbol + 1) * (state->max_symbol + 1);
        
        for (uint32_t i = 0; i < table_size; i++) {
            if (state->pair_freq[i] > max_freq) {
                max_freq = state->pair_freq[i];
                best_a = i / (state->max_symbol + 1);
                best_b = i % (state->max_symbol + 1);
            }
        }
        
        // Stop if no frequent pair found
        if (max_freq < 2) break;
        
        // Create new symbol for this pair
        uint32_t new_symbol = ++state->max_symbol;
        GrammarRule* rule = &state->grammar.rules[state->grammar.n_rules++];
        rule->lhs = new_symbol;
        rule->rhs1 = best_a;
        rule->rhs2 = best_b;
        rule->frequency = max_freq;
        
        // Replace all occurrences of the pair with new symbol
        size_t write_pos = 0;
        for (size_t i = 0; i < n_symbols; i++) {
            if (i < n_symbols - 1 && symbols[i] == best_a && symbols[i + 1] == best_b) {
                symbols[write_pos++] = new_symbol;
                i++;  // Skip next symbol (it's part of the pair)
            } else {
                symbols[write_pos++] = symbols[i];
            }
        }
        n_symbols = write_pos;
        n_iterations++;
        
        if (n_iterations % 100 == 0) {
            printf("[APEX] Iteration %u: created rule %u -> %u %u (freq=%u), %zu symbols remain\n",
                   n_iterations, new_symbol, best_a, best_b, max_freq, n_symbols);
        }
    }
    
    printf("[APEX] Grammar built: %u rules, %u terminals, %zu final symbols\n",
           state->grammar.n_rules, state->grammar.n_terminals, n_symbols);
    
    free(symbols);
}

// Initialize tANS encoder with uniform distribution
static void init_tans(tANSState* ans) {
    ans->state = 1 << 16;  // Initial state in middle of range
    ans->output_pos = 0;
    
    // Initialize symbol table with uniform probabilities
    ans->symbol_table = (uint32_t*)calloc(256, sizeof(uint32_t));
    for (int i = 0; i < 256; i++) {
        ans->symbol_table[i] = 4;  // Equal probability for all bytes
    }
    
    // Build state transition table
    ans->next_state = (uint32_t*)malloc((1 << 20) * sizeof(uint32_t));
    for (uint32_t s = 0; s < (1 << 20); s++) {
        // Simple transition: spread states across symbol space
        ans->next_state[s] = (s + 1) & ((1 << 20) - 1);
    }
}

// Encode single byte using tANS
static void tans_encode_byte(tANSState* ans, uint8_t byte) {
    // Simplified tANS encoding
    uint32_t prob = ans->symbol_table[byte];
    if (prob == 0) prob = 1;
    
    // Scale state based on probability
    ans->state = (ans->state / prob) * 256 + byte;
    
    // Flush state if it gets too large
    if (ans->state >= (1 << 24)) {
        if (ans->output_pos + 2 < ans->output_size) {
            ans->output[ans->output_pos++] = (ans->state >> 16) & 0xFF;
            ans->output[ans->output_pos++] = (ans->state >> 8) & 0xFF;
        }
        ans->state &= 0xFFFF;
    }
}

// Finalize tANS encoding
static void tans_finalize(tANSState* ans) {
    // Flush remaining state
    if (ans->output_pos + 4 < ans->output_size) {
        ans->output[ans->output_pos++] = (ans->state >> 24) & 0xFF;
        ans->output[ans->output_pos++] = (ans->state >> 16) & 0xFF;
        ans->output[ans->output_pos++] = (ans->state >> 8) & 0xFF;
        ans->output[ans->output_pos++] = ans->state & 0xFF;
    }
}

// Initialize APEX encoder
int apex_init(APEXState* state, const uint8_t* input, size_t input_size) {
    if (!state || !input || input_size == 0) return -1;
    
    memset(state, 0, sizeof(APEXState));
    state->input = (uint8_t*)malloc(input_size);
    if (!state->input) return -1;
    
    memcpy(state->input, input, input_size);
    state->input_size = input_size;
    
    // Allocate pair frequency table (dynamically sized based on input)
    uint32_t initial_table_size = 65536;  // 256 * 256 for initial
    state->pair_freq = (uint32_t*)calloc(initial_table_size, sizeof(uint32_t));
    if (!state->pair_freq) {
        free(state->input);
        return -1;
    }
    
    printf("[APEX] Initialized with %zu bytes input\n", input_size);
    return 0;
}

// Build Re-Pair grammar from input
int apex_build_grammar(APEXState* state) {
    if (!state || !state->input) return -1;
    build_repair_grammar(state);
    return 0;
}

// Encode grammar using tANS
int apex_encode_grammar(APEXState* state) {
    if (!state) return -1;
    
    // Allocate output buffer (input size should be sufficient)
    size_t output_size = state->input_size * 2 + 1024;
    state->ans.output = (uint8_t*)malloc(output_size);
    state->ans.output_size = output_size;
    
    if (!state->ans.output) return -1;
    
    init_tans(&state->ans);
    
    // Encode number of rules (2 bytes)
    tans_encode_byte(&state->ans, (state->grammar.n_rules >> 8) & 0xFF);
    tans_encode_byte(&state->ans, state->grammar.n_rules & 0xFF);
    
    // Encode each grammar rule
    for (uint32_t i = 0; i < state->grammar.n_rules; i++) {
        GrammarRule* rule = &state->grammar.rules[i];
        tans_encode_byte(&state->ans, (rule->lhs >> 8) & 0xFF);
        tans_encode_byte(&state->ans, rule->lhs & 0xFF);
        tans_encode_byte(&state->ans, (rule->rhs1 >> 8) & 0xFF);
        tans_encode_byte(&state->ans, rule->rhs1 & 0xFF);
        tans_encode_byte(&state->ans, (rule->rhs2 >> 8) & 0xFF);
        tans_encode_byte(&state->ans, rule->rhs2 & 0xFF);
    }
    
    tans_finalize(&state->ans);
    
    printf("[APEX] Encoded grammar: %zu bytes\n", state->ans.output_pos);
    return 0;
}

// Complete APEX compression
APEXResult apex_compress(const uint8_t* input, size_t input_size) {
    APEXResult result = {0};
    
    if (!input || input_size == 0) {
        result.ratio = 0.0;
        return result;
    }
    
    APEXState state;
    if (apex_init(&state, input, input_size) != 0) {
        return result;
    }
    
    // Build Re-Pair grammar
    apex_build_grammar(&state);
    
    // Encode with tANS
    apex_encode_grammar(&state);
    
    // The grammar/tANS encoder above is still used for analysis, but the
    // decoder path must be lossless today. Emit a simple framed raw payload
    // until the reversible grammar decoder is completed.
    result.size = APEX_FRAME_HEADER_SIZE + input_size;
    result.data = (uint8_t*)malloc(result.size);
    if (result.data) {
        memcpy(result.data, APEX_FRAME_MAGIC, 4);
        store_u64_le(result.data + 4, (uint64_t)input_size);
        memcpy(result.data + APEX_FRAME_HEADER_SIZE, input, input_size);
        result.ratio = (double)result.size / input_size;
        result.executable = false;  // Compressed data is not directly executable
    }
    
    printf("[APEX] Compression: %zu -> %zu bytes (ratio: %.3f)\n",
           input_size, result.size, result.ratio);
    
    apex_free(&state);
    return result;
}

// APEX decompression
uint8_t* apex_decompress(const uint8_t* compressed, size_t compressed_size,
                         size_t* output_size) {
    if (!compressed || compressed_size == 0 || !output_size) {
        return NULL;
    }

    if (compressed_size >= APEX_FRAME_HEADER_SIZE &&
        memcmp(compressed, APEX_FRAME_MAGIC, 4) == 0) {
        uint64_t raw_size = load_u64_le(compressed + 4);
        if (raw_size > SIZE_MAX ||
            compressed_size - APEX_FRAME_HEADER_SIZE < (size_t)raw_size) {
            *output_size = 0;
            return NULL;
        }

        uint8_t* output = (uint8_t*)malloc((size_t)raw_size);
        if (!output) {
            *output_size = 0;
            return NULL;
        }
        memcpy(output, compressed + APEX_FRAME_HEADER_SIZE, (size_t)raw_size);
        *output_size = (size_t)raw_size;
        return output;
    }

    uint8_t* output = (uint8_t*)malloc(compressed_size);
    if (!output) {
        *output_size = 0;
        return NULL;
    }
    memcpy(output, compressed, compressed_size);
    *output_size = compressed_size;
    return output;
}

// Free APEX state
void apex_free(APEXState* state) {
    if (!state) return;
    
    free(state->input);
    free(state->grammar.rules);
    free(state->pair_freq);
    free(state->ans.symbol_table);
    free(state->ans.next_state);
    free(state->ans.output);
    
    memset(state, 0, sizeof(APEXState));
}

// Free APEX result
void apex_result_free(APEXResult* result) {
    if (!result) return;
    free(result->data);
    result->data = NULL;
    result->size = 0;
}
