// bine/bine_internal.h — BINE: Binary Interaction Net Executor
// Raw Executable Superoptimization via Intrinsic Self-Description
// Zero External Data — Zero Tradeoffs — Directly Executable Output

#ifndef BINE_INTERNAL_H
#define BINE_INTERNAL_H

#include "bine_compat.h"
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
    uint8_t     type;      // STT_FUNC=2, STT_OBJECT=1, STT_NOTYPE=0
    uint8_t     binding;   // STB_LOCAL=0, STB_GLOBAL=1, STB_WEAK=2
} Symbol;

typedef struct {
    uint64_t    offset;    // Offset in .text or .data
    uint32_t    symbol_idx;
    int64_t     addend;
    uint32_t    type;      // R_X86_64_64=1, R_X86_64_PC32=2, etc.
} Reloc;

typedef struct {
    // From ELF header (self-declared)
    uint16_t    e_machine;  // architecture: EM_X86_64=62, EM_AARCH64=183, EM_RISCV=243
    uint64_t    e_entry;    // entry point address
    uint8_t     e_class;    // 32-bit or 64-bit (ELFCLASS64=2)
    
    // From program headers (self-declared execution spec)
    uint64_t    load_vaddr; // virtual address of executable LOAD segment
    uint64_t    load_offset;// file offset of executable LOAD segment
    uint64_t    load_size;  // size of executable LOAD segment (~.text size)

    // From section headers (self-declared byte-range purposes)
    uint8_t*    text_bytes; // .text section bytes (owned, must be freed)
    uint64_t    text_size;  // .text section size in bytes
    uint64_t    text_vaddr; // virtual address of .text section start
    uint8_t*    rodata_bytes; // .rodata section bytes (owned, must be freed)
    uint64_t    rodata_size;
    uint8_t*    data_bytes; // .data section bytes (owned, must be freed)
    uint64_t    data_size;
    
    // From symbol table (self-declared function boundaries)
    Symbol*     symbols;    // array of symbols (owned, must be freed)
    uint32_t    n_symbols;
    
    // From relocation table (self-declared address dependencies)
    Reloc*      relocs;     // array of relocations (owned, must be freed)
    uint32_t    n_relocs;
    
    // From dynamic section (self-declared external dependencies)
    char**      needed_libs;// NEEDED entries (owned, must be freed)
    uint32_t    n_needed;
    
    // From build ID note (self-declared identity hash)
    uint8_t     build_id[20];// GNU build ID (20 bytes SHA-1, or all zeros if absent)
    bool        has_build_id;
    
    // From .comment section (self-declared provenance)
    char*       compiler_str; // e.g., "GCC: (GNU) 13.2.1" (owned, must be freed)
    
    // Derived: file fingerprint (from inode metadata)
    uint64_t    fingerprint; // see Oracle 2
    
    // File path (for Oracle 3)
    char*       path;       // input file path (owned, must be freed)
    
} BinaryIntrinsic;

// ============================================================================
// STRAND Masks: Instruction-Level Oracle
// ============================================================================

typedef struct {
    bine_u128   M_R;        // Read mask: bits that vary and correlate with reads
    bine_u128   M_W;        // Write mask: bits that vary and correlate with writes
    bine_u128   M_K;        // Kill mask: bits with high XOR variance (state-changing)
    float       theta_R;    // Read entropy threshold
    float       theta_W;    // Write entropy threshold
    float       theta_K;    // Kill variance threshold
} StrandMasks;

// ============================================================================
// Interaction Net Agent Types
// ============================================================================

typedef enum {
    AGENT_CON = 0,      // Single-use computation
    AGENT_DUP = 1,      // Shared computation (fanout > 1)
    AGENT_ERA = 2,      // Dead computation (erasure)
    AGENT_SIFT = 3,     // Constant/leaf value
    AGENT_ATOMIC = 4    // Opaque region (self-mod code, inline asm)
} AgentType;

// P-bit state for STRAND-P (CUDA compatible)
typedef struct {
    float value;           // Current value (-1.0 to 1.0)
    float bias;            // Magnetic bias (coupling to external field)
    float temperature;     // Current temperature (0.0 = deterministic, 1.0 = random)
    uint64_t seed;         // Random seed for each p-bit
} PBitState;

// Agent array structure for GPU-ICOR (SoA - Structure of Arrays)
typedef struct {
    uint64_t* types;        // Agent type (CON=1, DUP=2, ERA=3, VAR=0)
    uint32_t* port0;        // Port 0 connection (agent index)
    uint32_t* port1;        // Port 1 connection (agent index)
    uint32_t* port2;        // Port 2 connection (agent index)
    uint32_t* hash;         // Hash for hash-consing
    uint32_t* generation;   // Generation for tracking changes
    uint32_t* active;       // Active flag (1 if alive, 0 if ERA)
    uint64_t* packed;       // Packed original representation (for GPU net format preservation)
    uint32_t n_agents;      // Total number of agents
} AgentArray;

// Agent structure (64-bit packed format for GPU)
typedef struct {
    uint32_t type      : 4;
    uint32_t port_0    : 16;
    uint32_t port_1    : 16;
    uint32_t port_2    : 16;
    uint32_t port_3    : 8;
    uint32_t flags     : 3;
    uint32_t essential : 1;  // Mark as essential (cannot be eliminated by ICOR)
} AgentPacked;

// ============================================================================
// BINE API
// ============================================================================

// Stage 1: Extract complete self-description from ELF binary
BinaryIntrinsic* bine_extract_intrinsic(const char* path);
void bine_free_intrinsic(BinaryIntrinsic* bin);

// GPU binary intrinsic extraction (CUDA PTX/cubin)
typedef struct {
    char* path;
    uint64_t file_size;
    char* ptx_text;
    uint64_t ptx_size;
    struct {
        char name[256];
        uint64_t offset;
        uint32_t insn_count;
        uint32_t reg_count;
    }* kernels;
    uint32_t n_kernels;
    uint32_t total_insns;
    uint32_t max_regs;
} GPUIntrinsic;

GPUIntrinsic* bine_extract_gpu_intrinsic(const char* path);
void bine_free_gpu_intrinsic(GPUIntrinsic* gpu);

// ============================================================================
// Instruction Decoder
// ============================================================================

typedef struct {
    uint64_t offset;
    uint8_t  length;
    uint8_t  opcode[15];
    bool     is_branch;
    bool     is_call;
    bool     is_ret;
    bool     predicated; // PTX @p branch / predicated control flow
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

// Decode all instructions in a byte array
Instruction* decode_instructions(const uint8_t* text, uint64_t text_size, uint32_t* n_insns);

void free_instructions(Instruction* insns);

// GPU CFG construction
typedef struct {
    uint64_t start_insn;
    uint64_t end_insn;
    uint32_t kernel_id;
    uint8_t is_entry;
    uint8_t is_exit;
    uint32_t* successors;
    uint32_t n_succ;
    uint32_t* predecessors;
    uint32_t n_pred;
} GPUCFGBlock;

typedef struct {
    GPUCFGBlock* blocks;
    uint32_t n_blocks;
    uint32_t n_kernels;
    uint32_t* kernel_start;
} GPUCFG;

GPUCFG* bine_build_gpu_cfg(const GPUIntrinsic* gpu, const Instruction* insns, uint32_t n_insns);
void bine_free_gpu_cfg(GPUCFG* cfg);
int bine_gpu_analyze_reachability(const GPUCFG* cfg, bool* reachable);

// GPU DFG construction
typedef struct {
    uint64_t insn_idx;
    uint32_t kernel_id;
    uint32_t* use_regs;
    uint32_t* def_regs;
    uint32_t n_use;
    uint32_t n_def;
    uint32_t* consumers;
    uint32_t n_consumers;
    uint32_t* producers;
    uint32_t n_producers;
} GPUDFGNode;

typedef struct {
    GPUDFGNode* nodes;
    uint32_t n_nodes;
    uint32_t n_kernels;
    uint32_t* kernel_offsets;
} GPUDFG;

GPUDFG* bine_build_gpu_dfg(const GPUIntrinsic* gpu, const Instruction* insns, uint32_t n_insns);
void bine_free_gpu_dfg(GPUDFG* dfg);

// GPU interaction net construction
typedef struct {
    uint64_t id;
    uint8_t type;
    uint32_t kernel_id;
    uint64_t insn_idx;
    uint8_t essential;
    uint8_t simt_aware;
    uint32_t wire_0;
    uint32_t wire_1;
    uint32_t wire_2;
    uint32_t consumers[8];
    uint32_t n_consumers;
} GPUAgent;

typedef struct {
    GPUAgent* agents;
    uint32_t n_agents;
    uint32_t n_kernels;
    uint32_t root_wire;
    uint64_t* wire_to_agent;
} GPUNet;

GPUNet* bine_build_gpu_net(const GPUIntrinsic* gpu, const Instruction* insns, uint32_t n_insns, const GPUDFG* dfg, const bool* reachable_insns);
void bine_free_gpu_net(GPUNet* net);
uint64_t* bine_pack_gpu_net(const GPUNet* net, uint32_t* n_packed);

// GPU PTX serialization
int bine_serialize_gpu_ptx(const uint64_t* packed_net, uint32_t n_agents, const GPUIntrinsic* original, const char* output_path);
int bine_compile_gpu_cubin(const char* ptx_path, const char* cubin_path);

// Binary to ICOR conversion
uint64_t* binary_to_icor_net(const uint8_t* binary, size_t size, uint32_t* n_agents);
uint8_t* icor_net_to_binary(const uint64_t* net, uint32_t n_agents, size_t* size);

// ICOR serialization to executable binary
int icor_serialize_to_binary(const uint64_t* net, uint32_t n_agents, const char* output_path);
uint64_t* icor_deserialize_from_binary(const char* input_path, uint32_t* n_agents);
int icor_execute_normal_form(const uint64_t* net, uint32_t n_agents, uint8_t* input_data, size_t input_size, uint8_t* output_data, size_t* output_size);
int icor_to_elf(const uint64_t* net, uint32_t n_agents, const char* output_path, uint16_t machine_type);

// Full binary optimization pipeline
uint8_t* bine_optimize_binary(const uint8_t* input, size_t input_size, size_t* output_size, float* reduction_ratio);

// PTX instruction decoding
Instruction* decode_ptx_instructions(const GPUIntrinsic* gpu, uint32_t* n_insns_out);

// After decode_ptx_instructions: scale per-kernel insn_count so sums match n_decoded (parser alignment).
void bine_align_gpu_insn_counts(GPUIntrinsic* gpu, uint32_t n_decoded);

// Stage 2: Derive STRAND masks from bit entropy
StrandMasks bine_derive_masks(const uint8_t* text, uint64_t size);

// Stage 3: Build interaction net from binary
uint64_t* bine_build_net(const BinaryIntrinsic* bin, const StrandMasks* masks,
                         uint32_t* n_agents, uint32_t* root_wire);
void bine_free_net(uint64_t* net);

// Stage 4: GPU-ICOR reduction
uint64_t* bine_icor_reduce(const uint64_t* net, uint32_t n_agents,
                          uint32_t* n_reduced, uint32_t* rounds);

// Stage 5: STRAND-P joint optimization (produces a schedule order; the net
// itself is never renumbered — safe for the ELF pipeline)
int bine_strand_p_optimize(const uint64_t* net, uint32_t n_agents,
                           const StrandMasks* masks,
                           uint32_t** out_order, uint32_t* n_scheduled);

// Stage 6: Serialize net back to ELF
int bine_serialize_elf(const uint64_t* net, uint32_t n_agents,
                      const BinaryIntrinsic* original,
                      const char* output_path);

// Stage 6b: Serialize with a STRAND-P schedule order (offset-preserving
// DCE + safe same-length reordering; schedule_order may be NULL).
int bine_serialize_elf_ex(const uint64_t* net, uint32_t n_agents,
                          const uint32_t* schedule_order, uint32_t n_scheduled,
                          const BinaryIntrinsic* original,
                          const char* output_path);

// Slot-stable host ICOR reduction (ELF pipeline; never renumbers agents)
int bine_icor_reduce_cpu(const uint64_t* net, uint32_t n_agents,
                         uint64_t** out_net, uint32_t* n_out,
                         uint32_t* rounds);
// Is agent i a live (non-ERA, non-dead) instruction after reduction?
int bine_icor_agent_live(const uint64_t* reduced, uint32_t n_reduced,
                         uint32_t i);

// Host STRAND-P scheduler: produces a schedule order (permutation of
// 0..n-1, earlier = higher priority), never renumbers the net.
int bine_strand_p_schedule_cpu(uint32_t n_instructions,
                               const uint8_t* masks_r,
                               const uint8_t* masks_w,
                               const uint8_t* masks_k,
                               uint32_t* schedule_order);

// Offset-preserving layout engine (serialize/layout.c)
uint8_t* bine_compute_dead_agents(const uint64_t* net, uint32_t n_agents,
                                  const Instruction* insns, uint32_t n_insns,
                                  const BinaryIntrinsic* bin);
int bine_apply_layout(const uint64_t* net, uint32_t n_agents,
                      const uint32_t* schedule_order, uint32_t n_scheduled,
                      const BinaryIntrinsic* original,
                      uint8_t* out, uint64_t* out_size,
                      uint64_t** offset_map_out);

// Instruction encoders
uint32_t encode_agent_x86_64(uint64_t agent, uint32_t reg_assign, uint8_t* out);
uint32_t encode_agent_aarch64(uint64_t agent, uint32_t reg_assign, uint8_t* out);
uint32_t encode_agent_riscv(uint64_t agent, uint32_t reg_assign, uint8_t* out);

// GPU register assignment
uint32_t* bine_assign_registers(const uint64_t* net, uint32_t n_agents,
                                const BinaryIntrinsic* bin, uint32_t* n_assigned);

// ============================================================================
// Instruction Decoder
// ============================================================================

Instruction* decode_instructions(const uint8_t* text, uint64_t text_size, uint32_t* n_insns);
void free_instructions(Instruction* insns);

// x86-64 length decoder (src/binary/length.c): exact instruction length at
// p (0 on undecodable input).  Used for boundary-safe decoding.
uint32_t x86_64_insn_length(const uint8_t* p, uint32_t max_len);

// Boundary-safe decoder (src/binary/decode.c): length decoder drives the
// instruction boundaries; semantic fields are filled where valid.
Instruction* decode_instructions_safe(const uint8_t* text, uint64_t text_size,
                                      uint32_t* n_insns);

// NOP detection (src/binary/pattern.c): collects NOP instruction offsets.
uint64_t detect_nops(const uint8_t* text, uint64_t text_size,
                     uint64_t* nop_offsets, uint64_t max_nops);

// BasicBlock structure for CFG
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

// CFG analysis for control flow preservation
bool* bine_analyze_reachability(const BasicBlock* cfg, uint32_t n_cfg,
                                const Instruction* insns, uint32_t n_insns,
                                const BinaryIntrinsic* bin);

// Relocation adjustment
int bine_adjust_relocations(uint8_t* elf_buffer, uint64_t elf_size,
                           uint64_t old_text_size, uint64_t new_text_size,
                           const uint64_t* offset_map, uint32_t n_insns,
                           const Instruction* insns);

// Peephole optimization (in-place .text transforms)
int bine_peephole_optimize(uint8_t* text, uint64_t text_size, uint64_t* new_size_out);

// ELF compaction (strip debug/note/SHT, truncate trailing padding)
int bine_compact_elf(const char* elf_path, const char* output_path);

// CUBIN optimization and compaction
int bine_optimize_cubin(const char* input_path, const char* output_path);
int bine_optimize_gpu(const char* input_path, const char* output_path);

// Main API functions
int bine_optimize(const char* input_path, const char* output_path);
int bine_verify(const char* original_path, const char* optimized_path,
                const char* test_input);
int bine_info(const char* binary_path);
int bine_bench(const char* input_path, const char* output_path);

// ============================================================================
// Utility Functions
// ============================================================================

// FNV-1a 64-bit hash (for fingerprinting)
uint64_t fnv1a_64(const uint8_t* data, uint64_t len);

// Get file fingerprint from inode metadata
uint64_t bine_get_fingerprint(const char* path);

// Cache functions
int bine_cache_lookup(uint64_t fingerprint, uint64_t** cached_net, uint32_t* n_cached);
int bine_cache_store(uint64_t fingerprint, const uint64_t* net, uint32_t n_agents);

#ifdef __cplusplus
}
#endif

#endif // BINE_INTERNAL_H
