// bine/serialize/net_to_elf.c — Stage 6: Net → Optimized Binary Serialization
// Converts ICOR normal form net back to a valid ELF binary.

#include "bine.h"
#include <elf.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <string.h>

// Multi-byte NOP encodings for x86-64 (Phase 1: NOP replacement preserves offsets)
static const uint8_t nop_encodings[15][15] = {
    {0x90},                                    // 1-byte NOP
    {0x66, 0x90},                             // 2-byte NOP
    {0x0F, 0x1F, 0x00},                       // 3-byte NOP
    {0x0F, 0x1F, 0x40, 0x00},                 // 4-byte NOP
    {0x0F, 0x1F, 0x44, 0x00, 0x00},           // 5-byte NOP
    {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},     // 6-byte NOP
    {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00}, // 7-byte NOP
    {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, // 8-byte NOP
    {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, // 9-byte NOP
    {0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, // 10-byte NOP
    {0x66, 0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, // 11-byte NOP
    {0x66, 0x66, 0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, // 12-byte NOP
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, // 13-byte NOP
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, // 14-byte NOP
    {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00}, // 15-byte NOP
};

static const uint8_t nop_len[15] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

// Emit a multi-byte NOP sequence of exactly 'len' bytes
static void emit_nop(uint8_t* dst, uint8_t len) {
    while (len > 0) {
        uint8_t n = (len > 15) ? 15 : len;
        memcpy(dst, nop_encodings[n - 1], n);
        dst += n;
        len -= n;
    }
}

// Serialize net to instruction bytes with dead code elimination
static int serialize_net_to_text(const uint64_t* net, uint32_t n_agents,
                                 uint8_t** text_out, uint64_t* text_size_out,
                                 const BinaryIntrinsic* original,
                                 const uint32_t* reg_assign,
                                 uint64_t** offset_map_out,
                                 Instruction** insns_out, uint32_t* n_insns_out) {
    (void)reg_assign;

    // Decode original instructions
    uint32_t n_insns = 0;
    Instruction* insns = decode_instructions(original->text_bytes, original->text_size, &n_insns);
    if (!insns || n_insns == 0) {
        *text_size_out = original->text_size;
        *text_out = (uint8_t*)malloc(original->text_size);
        if (!*text_out) return -1;
        memcpy(*text_out, original->text_bytes, original->text_size);
        *offset_map_out = (uint64_t*)calloc(n_agents, sizeof(uint64_t));
        *insns_out = NULL;
        *n_insns_out = 0;
        return 0;
    }

    // Safety: if decoder didn't cover all bytes, fall back to original
    uint64_t decoded_end = (n_insns > 0) ? insns[n_insns-1].offset + insns[n_insns-1].length : 0;
    if (decoded_end < original->text_size) {
        fprintf(stderr, "[BINE-SERIALIZE] Decoder covered %lu/%lu bytes, falling back to original\n",
                decoded_end, original->text_size);
        *text_size_out = original->text_size;
        *text_out = (uint8_t*)malloc(original->text_size);
        if (!*text_out) { free_instructions(insns); return -1; }
        memcpy(*text_out, original->text_bytes, original->text_size);
        free_instructions(insns);
        *offset_map_out = NULL;
        *insns_out = NULL;
        *n_insns_out = 0;
        return 0;
    }

    // Count non-ERA agents
    uint32_t live_agents = 0;
    for (uint32_t i = 0; i < n_agents; i++) {
        AgentPacked ap = *(AgentPacked*)&net[i];
        if (ap.type != AGENT_ERA) live_agents++;
    }

    // If no optimizations needed, copy original
    if (live_agents == n_insns) {
        *text_size_out = original->text_size;
        *text_out = (uint8_t*)malloc(original->text_size);
        if (!*text_out) {
            free_instructions(insns);
            return -1;
        }
        memcpy(*text_out, original->text_bytes, original->text_size);
        free_instructions(insns);
        *offset_map_out = NULL;
        *insns_out = NULL;
        *n_insns_out = 0;
        return 0;
    }

    // Allocate output buffer
    uint64_t max_size = original->text_size;
    *text_out = (uint8_t*)malloc(max_size);
    if (!*text_out) {
        free_instructions(insns);
        return -1;
    }

    // Allocate offset map
    *offset_map_out = (uint64_t*)calloc(n_insns, sizeof(uint64_t));
    if (!*offset_map_out) {
        free(*text_out);
        free_instructions(insns);
        return -1;
    }

    // Determine which instructions are dead
    uint8_t* is_dead = (uint8_t*)calloc(n_insns, sizeof(uint8_t));
    for (uint32_t i = 0; i < n_insns && i < n_agents; i++) {
        if (((AgentPacked*)&net[i])->type == AGENT_ERA) {
            is_dead[i] = 1;
        }
    }

    // Phase 1: NOP replacement (safe, preserves offsets)
    uint64_t offset = 0;
    uint32_t n_noped = 0;
    for (uint32_t i = 0; i < n_insns; i++) {
        uint64_t insn_len;
        if (i + 1 < n_insns) insn_len = insns[i + 1].offset - insns[i].offset;
        else insn_len = original->text_size - insns[i].offset;

        if (is_dead[i]) {
            // Safety: skip suspiciously large instructions (likely decoding errors)
            if (insn_len > 50 || insn_len == 0) {
                memcpy(*text_out + offset, original->text_bytes + insns[i].offset, insn_len);
            } else {
                emit_nop(*text_out + offset, (uint8_t)insn_len);
                n_noped++;
            }
        } else {
            memcpy(*text_out + offset, original->text_bytes + insns[i].offset, insn_len);
        }
        (*offset_map_out)[i] = offset;
        offset += insn_len;
    }

    free(is_dead);

    *text_size_out = offset;
    *insns_out = insns;
    *n_insns_out = n_insns;
    return 0;
}

// Main serialization function
int bine_serialize_elf(const uint64_t* net, uint32_t n_agents,
                      const BinaryIntrinsic* original,
                      const char* output_path) {
    if (!net || !original || !output_path) {
        return -1;
    }

    uint32_t era_count = 0;
    for (uint32_t i = 0; i < n_agents; i++) {
        AgentPacked ap = *(AgentPacked*)&net[i];
        if (ap.type == AGENT_ERA) era_count++;
    }
    fprintf(stderr, "[BINE-SERIALIZE] Net size: %u agents, ERA (dead) count: %u\n", n_agents, era_count);

    uint32_t n_assigned = 0;
    uint32_t* reg_assign = bine_assign_registers(net, n_agents, original, &n_assigned);

    uint8_t* text_bytes = NULL;
    uint64_t text_size = 0;
    uint64_t* offset_map = NULL;
    Instruction* insns = NULL;
    uint32_t n_insns = 0;
    if (serialize_net_to_text(net, n_agents, &text_bytes, &text_size, original,
                              reg_assign, &offset_map, &insns, &n_insns) < 0) {
        free(reg_assign);
        return -1;
    }

    free(reg_assign);

    // If no instructions decoded, copy original
    if (!insns || n_insns == 0) {
        free(text_bytes);
        free(offset_map);

        int src_fd = open(original->path, O_RDONLY);
        if (src_fd < 0) return -1;
        struct stat st;
        if (fstat(src_fd, &st) < 0) { close(src_fd); return -1; }
        uint8_t* buffer = (uint8_t*)malloc(st.st_size);
        if (!buffer) { close(src_fd); return -1; }
        if (read(src_fd, buffer, st.st_size) != st.st_size) {
            free(buffer); close(src_fd); return -1;
        }
        close(src_fd);

        int dst_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
        if (dst_fd < 0) { free(buffer); return -1; }
        if (write(dst_fd, buffer, st.st_size) != st.st_size) {
            free(buffer); close(dst_fd); return -1;
        }
        free(buffer);
        close(dst_fd);
        return 0;
    }

    // Open original binary to preserve structure
    int src_fd = open(original->path, O_RDONLY);
    if (src_fd < 0) {
        free(text_bytes); free(offset_map); free_instructions(insns);
        return -1;
    }
    struct stat st;
    if (fstat(src_fd, &st) < 0) {
        close(src_fd);
        free(text_bytes); free(offset_map); free_instructions(insns);
        return -1;
    }
    uint8_t* buffer = (uint8_t*)malloc(st.st_size);
    if (!buffer) {
        close(src_fd);
        free(text_bytes); free(offset_map); free_instructions(insns);
        return -1;
    }
    if (read(src_fd, buffer, st.st_size) != st.st_size) {
        free(buffer); close(src_fd);
        free(text_bytes); free(offset_map); free_instructions(insns);
        return -1;
    }
    close(src_fd);

    // Adjust relocations
    bine_adjust_relocations(buffer, st.st_size, original->text_size, text_size,
                            offset_map, n_insns, insns);

    // Find .text section file offset
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)buffer;
    uint64_t text_offset = original->load_offset;
    if (ehdr->e_shoff != 0 && ehdr->e_shnum > 0) {
        Elf64_Shdr* shdrs = (Elf64_Shdr*)(buffer + ehdr->e_shoff);
        char* shstrtab = NULL;
        if (ehdr->e_shstrndx != SHN_UNDEF) {
            shstrtab = (char*)(buffer + shdrs[ehdr->e_shstrndx].sh_offset);
        }
        for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
            const char* name = shstrtab ? shstrtab + shdrs[i].sh_name : "";
            if (strcmp(name, ".text") == 0) {
                text_offset = shdrs[i].sh_offset;
                break;
            }
        }
    }

    // Write optimized .text
    if (text_offset + text_size <= (uint64_t)st.st_size) {
        memcpy(buffer + text_offset, text_bytes, text_size);
    }

    // Preserve .rodata
    if (original->rodata_bytes && original->rodata_size > 0 && ehdr->e_shoff != 0) {
        Elf64_Shdr* shdrs = (Elf64_Shdr*)(buffer + ehdr->e_shoff);
        char* shstrtab = NULL;
        if (ehdr->e_shstrndx != SHN_UNDEF) {
            shstrtab = (char*)(buffer + shdrs[ehdr->e_shstrndx].sh_offset);
        }
        for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
            const char* name = shstrtab ? shstrtab + shdrs[i].sh_name : "";
            if (strcmp(name, ".rodata") == 0) {
                uint64_t rodata_offset = shdrs[i].sh_offset;
                if (rodata_offset + original->rodata_size <= st.st_size) {
                    memcpy(buffer + rodata_offset, original->rodata_bytes, original->rodata_size);
                }
                break;
            }
        }
    }

    if (text_size != original->text_size) {
        fprintf(stderr,
                "[BINE-SERIALIZE] Refusing size-changing .text rewrite (%lu -> %lu) without layout relocation\n",
                original->text_size, text_size);
        free(buffer);
        free(text_bytes);
        free(offset_map);
        free_instructions(insns);
        return -1;
    }

    // Update .text section header
    if (ehdr->e_shoff != 0) {
        Elf64_Shdr* shdrs = (Elf64_Shdr*)(buffer + ehdr->e_shoff);
        char* shstrtab = NULL;
        if (ehdr->e_shstrndx != SHN_UNDEF) {
            shstrtab = (char*)(buffer + shdrs[ehdr->e_shstrndx].sh_offset);
        }
        for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
            const char* name = shstrtab ? shstrtab + shdrs[i].sh_name : "";
            if (strcmp(name, ".text") == 0) {
                shdrs[i].sh_size = text_size;
                break;
            }
        }
    }

    // Write output file
    int dst_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (dst_fd < 0) {
        free(buffer); free(text_bytes); free(offset_map); free_instructions(insns);
        return -1;
    }
    if (write(dst_fd, buffer, st.st_size) != (ssize_t)st.st_size) {
        free(buffer); free(text_bytes); free(offset_map); free_instructions(insns);
        close(dst_fd);
        return -1;
    }

    free(buffer);
    free(text_bytes);
    free(offset_map);
    free_instructions(insns);
    close(dst_fd);
    return 0;
}
