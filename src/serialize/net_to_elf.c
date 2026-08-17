// bine/serialize/net_to_elf.c — Stage 6: Net → Optimized Binary Serialization
// Offset-preserving ELF rewrite: dead code becomes multi-byte NOPs and the
// STRAND-P schedule is realized with provably-safe same-length swaps (see
// serialize/layout.c).  .text size never changes, so section offsets, symbol
// values, relocation offsets and branch displacements all stay valid.

#include "bine.h"
#include "icor/bits.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Serialize net to instruction bytes with dead code elimination
static int serialize_net_to_text(const uint64_t* net, uint32_t n_agents,
                                 const uint32_t* schedule_order,
                                 uint32_t n_scheduled,
                                 uint8_t** text_out, uint64_t* text_size_out,
                                 uint64_t** offset_map_out,
                                 const BinaryIntrinsic* original) {
    uint8_t* text = (uint8_t*)malloc(original->text_size);
    if (!text) return -1;

    uint64_t new_size = 0;
    uint64_t* offset_map = NULL;
    int n_insns = bine_apply_layout(net, n_agents, schedule_order, n_scheduled,
                                    original, text, &new_size, &offset_map);
    if (n_insns < 0) {
        free(text);
        return -1;
    }

    *text_out = text;
    *text_size_out = new_size;
    *offset_map_out = offset_map;
    return 0;
}

// Main serialization function (with STRAND-P schedule order)
int bine_serialize_elf_ex(const uint64_t* net, uint32_t n_agents,
                          const uint32_t* schedule_order, uint32_t n_scheduled,
                          const BinaryIntrinsic* original,
                          const char* output_path) {
    if (!net || !original || !output_path) {
        return -1;
    }

    uint32_t era_count = 0;
    for (uint32_t i = 0; i < n_agents; i++) {
        if (WORD_TYPE(net[i]) == 2) era_count++;
    }
    fprintf(stderr, "[BINE-SERIALIZE] Net size: %u agents, ERA (dead) count: %u\n",
            n_agents, era_count);

    uint8_t* text_bytes = NULL;
    uint64_t text_size = 0;
    uint64_t* offset_map = NULL;
    if (serialize_net_to_text(net, n_agents, schedule_order, n_scheduled,
                              &text_bytes, &text_size, &offset_map,
                              original) < 0) {
        fprintf(stderr, "[BINE-SERIALIZE] Layout failed, copying original\n");
        return bine_file_copy(original->path, output_path);
    }

    // Decode failed or nothing to change: copy original verbatim.
    if (text_size == original->text_size &&
        memcmp(text_bytes, original->text_bytes, text_size) == 0) {
        free(text_bytes);
        free(offset_map);
        return bine_file_copy(original->path, output_path);
    }

    // Load the original ELF image
    uint8_t* buffer = NULL;
    size_t elf_size = 0;
    if (bine_read_file(original->path, &buffer, &elf_size) != 0) {
        free(text_bytes);
        free(offset_map);
        return -1;
    }

    // Safety: layout must be offset-preserving (identical total size).
    if (text_size != original->text_size) {
        fprintf(stderr,
                "[BINE-SERIALIZE] Refusing size-changing .text rewrite (%llu -> %llu)\n",
                (unsigned long long)original->text_size,
                (unsigned long long)text_size);
        free(buffer);
        free(text_bytes);
        free(offset_map);
        return -1;
    }

    // Find .text section file offset and patch bytes + sh_size.
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)buffer;
    uint64_t text_offset = original->load_offset;
    if (ehdr->e_shoff != 0 && ehdr->e_shnum > 0 &&
        ehdr->e_shoff + (uint64_t)ehdr->e_shnum * sizeof(Elf64_Shdr) <= elf_size) {
        Elf64_Shdr* shdrs = (Elf64_Shdr*)(buffer + ehdr->e_shoff);
        char* shstrtab = NULL;
        if (ehdr->e_shstrndx != SHN_UNDEF && ehdr->e_shstrndx < ehdr->e_shnum) {
            shstrtab = (char*)(buffer + shdrs[ehdr->e_shstrndx].sh_offset);
        }
        for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
            const char* name = shstrtab ? shstrtab + shdrs[i].sh_name : "";
            if (strcmp(name, ".text") == 0) {
                text_offset = shdrs[i].sh_offset;
                shdrs[i].sh_size = text_size;
                break;
            }
        }
    }

    // Write optimized .text (offsets preserved, so relocations and symbols
    // need no adjustment).
    if (text_offset + text_size <= (uint64_t)elf_size) {
        memcpy(buffer + text_offset, text_bytes, text_size);
    } else {
        free(buffer);
        free(text_bytes);
        free(offset_map);
        return -1;
    }

    int rc = bine_write_file(output_path, buffer, elf_size);

    free(buffer);
    free(text_bytes);
    free(offset_map);
    return rc;
}

// Main serialization function (backward-compatible: no schedule order)
int bine_serialize_elf(const uint64_t* net, uint32_t n_agents,
                      const BinaryIntrinsic* original,
                      const char* output_path) {
    return bine_serialize_elf_ex(net, n_agents, NULL, 0, original, output_path);
}
