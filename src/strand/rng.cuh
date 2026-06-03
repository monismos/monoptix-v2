// bine/strand/rng.cuh — Deterministic SplitMix64 RNG for GPU kernels
// Replaces cuRAND for reproducible, seedable random number generation.
// SplitMix64: https://xoshiro.di.unimi.it/splitmix64.c

#ifndef BINE_RNG_CUH
#define BINE_RNG_CUH

#include <cstdint>

struct SplitMix64State {
    uint64_t state;
};

__device__ __forceinline__ uint64_t splitmix64_next(uint64_t* state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// Uniform float in [0, 1)
__device__ __forceinline__ float splitmix64_uniform(uint64_t* state) {
    uint64_t v = splitmix64_next(state);
    return (float)(v >> 40) / (float)(1ULL << 24);
}

// Initialize SplitMix64 state from a seed + thread index
__device__ __forceinline__ uint64_t splitmix64_seed(uint64_t base_seed, uint32_t tid) {
    uint64_t z = base_seed + (uint64_t)tid;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// SHA-256-like mix for deriving a 64-bit seed from arbitrary bytes
// (used on the host to hash the input binary path/fingerprint)
__host__ __forceinline__ uint64_t bine_hash_seed(const uint8_t* data, uint32_t len) {
    uint64_t h = 0xcbf29ce484222325ull; // FNV offset basis
    for (uint32_t i = 0; i < len; i++) {
        h ^= (uint64_t)data[i];
        h *= 0x100000001b3ull; // FNV prime
    }
    // Finalize with SplitMix64 mixing
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
    return h ^ (h >> 31);
}

#endif // BINE_RNG_CUH
