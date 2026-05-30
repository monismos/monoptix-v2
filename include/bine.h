#ifndef BINE_H
#define BINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// BinaryIntrinsic: Complete Self-Description from ELF Binary
// ============================================================================

typedef struct {
    const char* name;
    uint64_t    address;
    uint64_t    size;
    uint8_t     type;
    uint8_t     binding;
} Symbol;

typedef struct {
    uint64_t    offset;
    uint32_t    symbol_idx;
    int64_t     addend;
    uint32_t    type;
} Reloc;

typedef struct {
    uint16_t    e_machine;
    uint64_t    e_entry;
    uint8_t     e_class;
    uint64_t    load_vaddr;
    uint64_t    load_offset;
    uint64_t    load_size;
    uint8_t*    text_bytes;
    uint64_t    text_size;
    uint64_t    text_vaddr;
    uint8_t*    rodata_bytes;
    uint64_t    rodata_size;
    uint8_t*    data_bytes;
    uint64_t    data_size;
    Symbol*     symbols;
    uint32_t    n_symbols;
    Reloc*      relocs;
    uint32_t    n_relocs;
    char**      needed_libs;
    uint32_t    n_needed;
    uint8_t     build_id[20];
    bool        has_build_id;
    char*       compiler_str;
    uint64_t    fingerprint;
    char*       path;
} BinaryIntrinsic;

// ============================================================================
// STRAND Masks
// ============================================================================

typedef struct {
    __uint128_t M_R;
    __uint128_t M_W;
    __uint128_t M_K;
    float       theta_R;
    float       theta_W;
    float       theta_K;
} StrandMasks;

typedef enum {
    AGENT_CON = 0,
    AGENT_DUP = 1,
    AGENT_ERA = 2,
    AGENT_SIFT = 3,
    AGENT_ATOMIC = 4
} AgentType;

typedef struct {
    float value;
    float bias;
    float temperature;
    uint64_t seed;
} PBitState;

typedef struct {
    uint64_t* types;
    uint32_t* port0;
    uint32_t* port1;
    uint32_t* port2;
    uint32_t* hash;
    uint32_t* generation;
    uint32_t* active;
    uint64_t* packed;
    uint32_t n_agents;
} AgentArray;

typedef struct {
    uint32_t type      : 4;
    uint32_t port_0    : 16;
    uint32_t port_1    : 16;
    uint32_t port_2    : 16;
    uint32_t port_3    : 8;
    uint32_t flags     : 3;
    uint32_t essential : 1;
} AgentPacked;

// ============================================================================
// Instruction types
// ============================================================================

typedef struct {
    uint64_t offset;
    uint8_t  length;
    uint8_t  opcode[15];
    bool     is_branch;
    bool     is_call;
    bool     is_ret;
    bool     predicated;
    bool     has_modrm;
    uint8_t  modrm;
    uint8_t  rex;
    uint8_t  reg;
    uint8_t  rm;
    int32_t  imm;
    uint64_t target;
    uint32_t def_reg;
    uint32_t use_regs[16];
    uint8_t  n_use_regs;
} Instruction;

typedef struct {
    uint64_t start;
    uint64_t end;
    uint32_t first_insn;
    uint32_t last_insn;
    uint32_t* successors;
    uint32_t n_successors;
    uint32_t* predecessors;
    uint32_t n_predecessors;
} BasicBlock;

// ============================================================================
// Core API
// ============================================================================

BinaryIntrinsic* bine_extract_intrinsic(const char* path);
void bine_free_intrinsic(BinaryIntrinsic* bin);
Instruction* decode_instructions(const uint8_t* text, uint64_t text_size, uint32_t* n_insns);
void free_instructions(Instruction* insns);
StrandMasks bine_derive_masks(const uint8_t* text, uint64_t size);
bool* bine_analyze_reachability(const BasicBlock* cfg, uint32_t n_cfg,
                                const Instruction* insns, uint32_t n_insns,
                                const BinaryIntrinsic* bin);
uint64_t* bine_build_net(const BinaryIntrinsic* bin, const StrandMasks* masks,
                         uint32_t* n_agents, uint32_t* root_wire);
void bine_free_net(uint64_t* net);
uint64_t* bine_icor_reduce(const uint64_t* net, uint32_t n_agents,
                          uint32_t* n_reduced, uint32_t* rounds);
uint64_t* bine_strand_p_optimize(const uint64_t* net, uint32_t n_agents,
                                 const StrandMasks* masks, uint32_t* n_scheduled);
int bine_serialize_elf(const uint64_t* net, uint32_t n_agents,
                       const BinaryIntrinsic* original, const char* output_path);
int bine_adjust_relocations(uint8_t* elf_buffer, uint64_t elf_size,
                            uint64_t old_text_size, uint64_t new_text_size,
                            const uint64_t* offset_map, uint32_t n_insns,
                            const Instruction* insns);
int bine_peephole_optimize(uint8_t* text, uint64_t text_size, uint64_t* new_size_out);
int bine_compact_elf(const char* elf_path, const char* output_path);
uint32_t* bine_assign_registers(const uint64_t* net, uint32_t n_agents,
                                const BinaryIntrinsic* bin, uint32_t* n_assigned);
uint32_t bine_serialize_net_to_text(const uint64_t* net, uint32_t n_agents,
                                    uint8_t** out_text, uint64_t* out_size,
                                    const BinaryIntrinsic* bin,
                                    const uint32_t* reg_assign,
                                    uint64_t** offset_map,
                                    Instruction** out_insns, uint32_t* n_insns);

// ============================================================================
// Public API
// ============================================================================

int bine_optimize(const char* input_path, const char* output_path);
int bine_verify(const char* original_path, const char* optimized_path, const char* test_input);
int bine_info(const char* binary_path);
int bine_bench(const char* input_path, const char* output_path);

// ============================================================================
// Binary-level analysis
// ============================================================================

void entropy_scan(const uint8_t* data, uint64_t size, float* per_byte_entropy, float* total_entropy);
uint64_t detect_nops(const uint8_t* text, uint64_t text_size, uint64_t* nop_offsets, uint64_t max_nops);
uint64_t detect_redundant_sequences(const uint8_t* data, uint64_t size,
                                    uint64_t min_length, uint64_t* offsets, uint64_t* lengths, uint64_t max_results);

// ============================================================================
// Grammar/compression
// ============================================================================

uint64_t* repair_compress(const uint8_t* data, uint64_t size, uint32_t* n_rules, uint64_t* compressed_size);
uint8_t* repair_decompress(const uint64_t* rules, uint32_t n_rules, uint64_t* output_size);

// ============================================================================
// NOP removal, merging, reordering passes
// ============================================================================

uint64_t bine_remove_nops(uint8_t* text, uint64_t text_size, uint8_t* out);
uint64_t bine_merge_sequences(const uint8_t* text, uint64_t text_size, uint8_t* out, uint64_t* offset_map);
uint64_t bine_reorder_code(const uint8_t* text, uint64_t text_size,
                           const BasicBlock* blocks, uint32_t n_blocks,
                           const bool* hotness, uint8_t* out);

// ============================================================================
// Entropy-based mask derivation (128-bit)
// ============================================================================

void derive_masks_128(const uint8_t* data, uint64_t size,
                      __uint128_t* M_R, __uint128_t* M_W, __uint128_t* M_K);

// ============================================================================
// Utility
// ============================================================================

uint64_t fnv1a_64(const uint8_t* data, uint64_t len);
uint64_t bine_get_fingerprint(const char* path);
int bine_cache_lookup(uint64_t fingerprint, uint64_t** cached_net, uint32_t* n_cached);
int bine_cache_store(uint64_t fingerprint, const uint64_t* net, uint32_t n_agents);

#ifdef __cplusplus
}
#endif

#endif
