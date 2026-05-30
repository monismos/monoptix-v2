// icor/result_encoding.cuh — Encode computation results in normal form
// Ensures results survive minimization by encoding them in the net structure

#ifndef RESULT_ENCODING_CUH
#define RESULT_ENCODING_CUH

#include <stdint.h>

// ============================================================================
// Result Encoding in Normal Form
// ============================================================================
// The key insight: encode the result as a CON-linked list that survives reduction
// CON(agent0, agent1, agent2) where agent2 points to the next character
// This structure is preserved through CON-CON annihilation rules

// Encode a string as a CON-linked list in the normal form
// Each character is encoded as a CON agent where:
// - port0: character value (in low bits)
// - port1: next character in chain (or 0 for end)
// - port2: unused (0)
// This structure survives CON-CON reduction because it's a proper linked list

__device__ __host__ static inline uint64_t encode_char_as_con(uint8_t c, uint32_t next) {
    // CON agent (type=0) with character in port0, next in port1
    // port0 = character value (20 bits, but we only use 8 bits)
    // port1 = next agent index
    // port2 = 0
    return (0ULL << 62) | ((uint64_t)c << 42) | ((uint64_t)next << 22) | 0ULL;
}

// Decode character from CON agent
__device__ __host__ static inline uint8_t decode_char_from_con(uint64_t w) {
    // Extract character from port0 (bits 42-61, but we only use low 8 bits)
    return (uint8_t)((w >> 42) & 0xFFULL);
}

// Get next pointer from CON agent
__device__ __host__ static inline uint32_t get_next_from_con(uint64_t w) {
    // Extract port1 (bits 22-41)
    return (uint32_t)((w >> 22) & 0xFFFFFULL);
}

// Check if agent is a character CON
__device__ __host__ static inline bool is_char_con(uint64_t w) {
    // Must be CON type (type=0) and have non-zero port0 (character)
    uint8_t type = (w >> 62) & 0x3ULL;
    uint32_t port0 = (w >> 42) & 0xFFFFFULL;
    return (type == 0) && (port0 != 0);
}

// ============================================================================
// GPU Kernel: Encode string as CON-linked list
// ============================================================================

__global__ void k_encode_string_as_con(
    uint8_t*  __restrict__ string,
    uint32_t              string_len,
    uint64_t* __restrict__ net,
    uint32_t              start_idx
) {
    uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= string_len) return;
    
    uint8_t c = string[i];
    uint32_t next = (i < string_len - 1) ? (start_idx + i + 1) : 0;
    
    // Encode character as CON agent
    net[start_idx + i] = encode_char_as_con(c, next);
}

// ============================================================================
// GPU Kernel: Decode string from CON-linked list in minimized net
// ============================================================================

__global__ void k_decode_string_from_con(
    uint64_t* __restrict__ net,
    uint32_t              n,
    uint32_t              start_idx,
    char*    __restrict__ output,
    uint32_t* __restrict__ output_len
) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid != 0) return;  // Single thread does the traversal
    
    uint32_t current = start_idx;
    uint32_t len = 0;
    
    // Follow the CON-linked list
    for (uint32_t i = 0; i < n && i < 256; i++) {
        if (current == 0 || current >= n) break;
        
        uint64_t w = net[current];
        
        // Check if this is a character CON
        if (!is_char_con(w)) break;
        
        // Extract character
        uint8_t c = decode_char_from_con(w);
        
        // Store in output
        if (len < 4096) {  // Buffer size limit
            output[len] = (char)c;
            len++;
        }
        
        // Get next pointer
        current = get_next_from_con(w);
    }
    
    *output_len = len;
}

// ============================================================================
// Host-side functions
// ============================================================================

// Encode a string as CON-linked list in the net
void encode_string_result(uint8_t* string, uint32_t string_len, 
                         uint64_t* net, uint32_t net_size, uint32_t start_idx) {
    // Upload string to GPU
    uint8_t* d_string;
    cudaMalloc(&d_string, string_len * sizeof(uint8_t));
    cudaMemcpy(d_string, string, string_len * sizeof(uint8_t), cudaMemcpyHostToDevice);
    
    // Upload net to GPU
    uint64_t* d_net;
    cudaMalloc(&d_net, net_size * sizeof(uint64_t));
    cudaMemcpy(d_net, net, net_size * sizeof(uint64_t), cudaMemcpyHostToDevice);
    
    // Encode string as CON-linked list
    k_encode_string_as_con<<<(string_len + 255) / 256, 256>>>(
        d_string, string_len, d_net, start_idx);
    cudaDeviceSynchronize();
    
    // Copy back to host
    cudaMemcpy(net, d_net, net_size * sizeof(uint64_t), cudaMemcpyDeviceToHost);
    
    // Cleanup
    cudaFree(d_string);
    cudaFree(d_net);
}

// Decode string from CON-linked list in the net
char* decode_string_result(uint64_t* net, uint32_t net_size, uint32_t start_idx, 
                          uint32_t* out_len) {
    // Upload net to GPU
    uint64_t* d_net;
    cudaMalloc(&d_net, net_size * sizeof(uint64_t));
    cudaMemcpy(d_net, net, net_size * sizeof(uint64_t), cudaMemcpyHostToDevice);
    
    // Allocate output buffer
    char* d_output;
    uint32_t* d_output_len;
    cudaMalloc(&d_output, 4096 * sizeof(char));
    cudaMalloc(&d_output_len, sizeof(uint32_t));
    
    // Decode string from CON-linked list
    k_decode_string_from_con<<<1, 1>>>(d_net, net_size, start_idx, d_output, d_output_len);
    cudaDeviceSynchronize();
    
    // Copy back to host
    char* h_output = (char*)malloc(4096);
    cudaMemcpy(h_output, d_output, 4096, cudaMemcpyDeviceToHost);
    cudaMemcpy(out_len, d_output_len, sizeof(uint32_t), cudaMemcpyDeviceToHost);
    
    // Cleanup
    cudaFree(d_net);
    cudaFree(d_output);
    cudaFree(d_output_len);
    
    return h_output;
}

#endif // RESULT_ENCODING_CUH
