// bine/serialize/reloc_adjust.c — ELF relocation adjustment
// Adjusts relocation entries when .text section size changes during optimization.

#include "bine.h"
#include <elf.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>

// Build byte-level offset map from instruction-level offset map
static uint64_t* build_byte_offset_map(const uint64_t* insn_offset_map, uint32_t n_insns,
                                       const Instruction* insns, uint64_t old_text_size,
                                       uint64_t new_text_size) {
    if (!insn_offset_map || !insns) return NULL;
    
    // Allocate byte-level map (original text size)
    uint64_t* byte_map = (uint64_t*)calloc(old_text_size, sizeof(uint64_t));
    if (!byte_map) return NULL;
    
    // Initialize with UINT64_MAX (invalid)
    for (uint64_t i = 0; i < old_text_size; i++) {
        byte_map[i] = UINT64_MAX;
    }
    
    // Map each instruction's bytes to new offsets
    uint64_t new_offset = 0;
    for (uint32_t i = 0; i < n_insns; i++) {
        if (insn_offset_map[i] == UINT64_MAX) {
            // Instruction was eliminated - skip
            continue;
        }
        
        uint64_t orig_start = insns[i].offset;
        uint64_t orig_end = orig_start + insns[i].length;
        uint64_t new_start = insn_offset_map[i];
        
        // Map each byte of this instruction
        for (uint64_t j = orig_start; j < orig_end && j < old_text_size; j++) {
            byte_map[j] = new_start + (j - orig_start);
        }
        
        new_offset += insns[i].length;
    }
    
    return byte_map;
}

// Adjust relocation entry using byte-level offset map
static int adjust_relocation_entry(Elf64_Rela* rela, const uint64_t* byte_map,
                                   uint64_t old_text_size, uint64_t new_text_size) {
    uint64_t r_offset = rela->r_offset;
    uint32_t r_type = ELF64_R_TYPE(rela->r_info);
    
    // Check if relocation is in .text section
    if (r_offset < old_text_size && byte_map) {
        // Map to new offset
        if (byte_map[r_offset] != UINT64_MAX) {
            uint64_t new_offset = byte_map[r_offset];
            rela->r_offset = new_offset;
        }
        // If byte was eliminated, keep original offset (may cause issues)
    }
    
    // Adjust based on relocation type
    switch (r_type) {
        case R_X86_64_64:
            // 64-bit absolute - offset adjustment is sufficient
            break;
        case R_X86_64_PC32:
        case R_X86_64_PLT32:
            // PC-relative - may need addend adjustment if target moved
            // For now, offset adjustment is sufficient
            break;
        case R_X86_64_32:
        case R_X86_64_32S:
            // 32-bit absolute - offset adjustment is sufficient
            break;
        case R_X86_64_GOTPCREL:
            // GOT PC-relative - offset adjustment is sufficient
            break;
        default:
            // Unknown type - leave as-is
            break;
    }
    
    return 0;
}

// Adjust all relocations in a relocation section
static int adjust_relocation_section(uint8_t* elf_buffer, uint64_t rela_offset,
                                   uint64_t rela_size, const uint64_t* byte_map,
                                   uint64_t old_text_size, uint64_t new_text_size) {
    uint64_t n_relocs = rela_size / sizeof(Elf64_Rela);
    Elf64_Rela* relas = (Elf64_Rela*)(elf_buffer + rela_offset);
    
    for (uint64_t i = 0; i < n_relocs; i++) {
        adjust_relocation_entry(&relas[i], byte_map, old_text_size, new_text_size);
    }
    
    return 0;
}

// Find and adjust all relocation sections
int bine_adjust_relocations(uint8_t* elf_buffer, uint64_t elf_size,
                           uint64_t old_text_size, uint64_t new_text_size,
                           const uint64_t* insn_offset_map, uint32_t n_insns,
                           const Instruction* insns) {
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)elf_buffer;
    
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) {
        return 0; // No section headers
    }
    
    Elf64_Shdr* shdrs = (Elf64_Shdr*)(elf_buffer + ehdr->e_shoff);
    
    // Build byte-level offset map
    uint64_t* byte_map = build_byte_offset_map(insn_offset_map, n_insns, insns,
                                              old_text_size, new_text_size);
    if (!byte_map) {
        return -1;
    }
    
    // Find string table for section names
    char* shstrtab = NULL;
    if (ehdr->e_shstrndx != SHN_UNDEF) {
        shstrtab = (char*)(elf_buffer + shdrs[ehdr->e_shstrndx].sh_offset);
    }
    
    // Iterate through sections to find .rela.* sections
    for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
        const char* name = shstrtab ? shstrtab + shdrs[i].sh_name : "";
        
        // Check if this is a relocation section
        if (shdrs[i].sh_type == SHT_RELA) {
            // Adjust this relocation section
            if (adjust_relocation_section(elf_buffer, shdrs[i].sh_offset,
                                        shdrs[i].sh_size, byte_map,
                                        old_text_size, new_text_size) < 0) {
                free(byte_map);
                return -1;
            }
        }
    }
    
    free(byte_map);
    return 0;
}
