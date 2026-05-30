// bine/apex/apex.h — APEX: Adaptive Pair Encoding with eXtensibility
// Full compression system combining tANS entropy coding with GIE-ICOR

#ifndef BINE_APEX_H
#define BINE_APEX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// tANS (asymmetric numeral systems) state
typedef struct {
    uint32_t state;           // Current tANS state
    uint32_t* symbol_table; // Symbol probability table
    uint32_t* next_state;   // State transition table
    uint8_t* output;        // Compressed output buffer
    size_t output_pos;      // Current position in output
    size_t output_size;     // Size of output buffer
} tANSState;

// GIE-ICOR grammar rule
typedef struct {
    uint32_t lhs;           // Left-hand side symbol
    uint32_t rhs1;          // First right-hand side symbol
    uint32_t rhs2;          // Second right-hand side symbol (0 if terminal)
    uint32_t frequency;     // How often this rule is used
} GrammarRule;

// GIE-ICOR grammar
typedef struct {
    GrammarRule* rules;     // Array of grammar rules
    uint32_t n_rules;       // Number of rules
    uint32_t n_terminals;   // Number of terminal symbols
    uint32_t start_symbol;  // Start symbol ID
} GIEGrammar;

// APEX encoder state
typedef struct {
    tANSState ans;          // tANS entropy coder state
    GIEGrammar grammar;     // GIE-ICOR grammar
    uint8_t* input;         // Input data to compress
    size_t input_size;      // Size of input data
    uint32_t* pair_freq;    // Pair frequency table for Re-Pair
    uint32_t max_symbol;    // Current maximum symbol ID
} APEXState;

// APEX compression result
typedef struct {
    uint8_t* data;          // Compressed data
    size_t size;            // Size of compressed data
    double ratio;           // Compression ratio achieved
    bool executable;        // True if output is directly executable
} APEXResult;

// Initialize APEX encoder
int apex_init(APEXState* state, const uint8_t* input, size_t input_size);

// Build Re-Pair grammar from input
int apex_build_grammar(APEXState* state);

// Encode grammar using tANS
int apex_encode_grammar(APEXState* state);

// Complete APEX compression
APEXResult apex_compress(const uint8_t* input, size_t input_size);

// APEX decompression
uint8_t* apex_decompress(const uint8_t* compressed, size_t compressed_size, 
                         size_t* output_size);

// Free APEX state
void apex_free(APEXState* state);

// Free APEX result
void apex_result_free(APEXResult* result);

// GIE-ICOR specific: convert binary to interaction net
uint64_t* binary_to_icor_net(const uint8_t* binary, size_t size, uint32_t* n_agents);

// GIE-ICOR specific: convert interaction net back to binary
uint8_t* icor_net_to_binary(const uint64_t* net, uint32_t n_agents, size_t* size);

#ifdef __cplusplus
}
#endif

#endif // BINE_APEX_H
