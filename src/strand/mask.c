// bine/strand/mask_oracle.c — Stage 2: STRAND Mask Oracle
// Derives M_R, M_W, M_K from bit entropy of the instruction stream.

#include "bine.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Helper to extract low 64 bits from __uint128_t
static uint64_t low128(__uint128_t x) {
    return (uint64_t)x;
}

// Helper to extract high 64 bits from __uint128_t
static uint64_t high128(__uint128_t x) {
    return (uint64_t)(x >> 64);
}

// Compute Bernoulli entropy for a bit position
static float bernoulli_entropy(uint32_t ones, uint32_t total) {
    if (total == 0) return 0.0f;
    float p = (float)ones / (float)total;
    if (p == 0.0f || p == 1.0f) return 0.0f;
    return -p * log2f(p) - (1.0f - p) * log2f(1.0f - p);
}

// Compute variance of XOR between consecutive instructions
static float xor_variance(const uint8_t* text, uint64_t size, int bit_pos) {
    if (size < 16) return 0.0f;
    
    uint64_t n_pairs = (size / 16) - 1;
    if (n_pairs == 0) return 0.0f;
    
    // Count XOR transitions
    uint32_t transitions = 0;
    for (uint64_t i = 0; i < n_pairs; i++) {
        const uint8_t* a = text + i * 16;
        const uint8_t* b = text + (i + 1) * 16;
        
        int byte_idx = bit_pos / 8;
        int bit_idx = bit_pos % 8;
        
        uint8_t a_bit = (a[byte_idx] >> bit_idx) & 1;
        uint8_t b_bit = (b[byte_idx] >> bit_idx) & 1;
        
        if (a_bit != b_bit) transitions++;
    }
    
    float mean = (float)transitions / (float)n_pairs;
    // Variance = E[X^2] - E[X]^2
    float e_x2 = (float)transitions / (float)n_pairs; // X is 0 or 1, so X^2 = X
    return e_x2 - mean * mean;
}

// Derive STRAND masks from bit entropy
StrandMasks bine_derive_masks(const uint8_t* text, uint64_t size) {
    StrandMasks masks;
    masks.M_R = 0;
    masks.M_W = 0;
    masks.M_K = 0;
    
    if (size < 16 || (size % 16) != 0) {
        // Invalid instruction stream
        masks.theta_R = 0.5f;
        masks.theta_W = 0.5f;
        masks.theta_K = 0.5f;
        return masks;
    }
    
    uint64_t n_insns = size / 16;
    
    // Count bit frequencies across all instructions
    uint32_t bit_ones[128] = {0};
    uint32_t bit_xor_transitions[128] = {0};
    
    for (uint64_t i = 0; i < n_insns; i++) {
        const uint8_t* insn = text + i * 16;
        
        for (int b = 0; b < 128; b++) {
            int byte_idx = b / 8;
            int bit_idx = b % 8;
            if (byte_idx < 16) {
                if ((insn[byte_idx] >> bit_idx) & 1) {
                    bit_ones[b]++;
                }
            }
        }
        
        // Count XOR transitions
        if (i > 0) {
            const uint8_t* prev = text + (i - 1) * 16;
            for (int b = 0; b < 128; b++) {
                int byte_idx = b / 8;
                int bit_idx = b % 8;
                if (byte_idx < 16) {
                    uint8_t prev_bit = (prev[byte_idx] >> bit_idx) & 1;
                    uint8_t curr_bit = (insn[byte_idx] >> bit_idx) & 1;
                    if (prev_bit != curr_bit) {
                        bit_xor_transitions[b]++;
                    }
                }
            }
        }
    }
    
    // Compute entropy for each bit position
    float entropy[128];
    float variance[128];
    
    for (int b = 0; b < 128; b++) {
        entropy[b] = bernoulli_entropy(bit_ones[b], n_insns);
        variance[b] = xor_variance(text, size, b);
    }
    
    // Set thresholds based on median entropy and variance
    float entropy_values[128];
    float variance_values[128];
    memcpy(entropy_values, entropy, sizeof(entropy));
    memcpy(variance_values, variance, sizeof(variance));
    
    // Simple median calculation (sort)
    for (int i = 0; i < 127; i++) {
        for (int j = i + 1; j < 128; j++) {
            if (entropy_values[i] > entropy_values[j]) {
                float tmp = entropy_values[i];
                entropy_values[i] = entropy_values[j];
                entropy_values[j] = tmp;
            }
        }
    }
    
    for (int i = 0; i < 127; i++) {
        for (int j = i + 1; j < 128; j++) {
            if (variance_values[i] > variance_values[j]) {
                float tmp = variance_values[i];
                variance_values[i] = variance_values[j];
                variance_values[j] = tmp;
            }
        }
    }
    
    float median_entropy = entropy_values[64];
    float median_variance = variance_values[64];
    
    masks.theta_R = median_entropy * 0.8f; // Slightly below median
    masks.theta_W = median_entropy * 0.8f;
    masks.theta_K = median_variance * 0.8f;
    
    // Classify bits based on entropy and correlation
    // For now, use a simple heuristic:
    // - High entropy bits = register fields (M_R or M_W)
    // - High variance XOR bits = state-changing (M_K)
    
    for (int b = 0; b < 128; b++) {
        if (entropy[b] > masks.theta_R) {
            // High entropy - could be read or write
            // For x86-64, lower bits of ModRM are typically source (read)
            // For ARM64, bits [9:5] are source, [4:0] are destination
            // Use position-based heuristic for now
            
            if (b < 32) {
                // Lower 32 bits often contain register fields
                masks.M_R |= (1ULL << b);
            } else if (b >= 32 && b < 64) {
                // Middle bits often contain register fields
                masks.M_W |= (1ULL << b);
            }
        }
        
        if (variance[b] > masks.theta_K) {
            masks.M_K |= (1ULL << b);
        }
    }
    
    
    return masks;
}
