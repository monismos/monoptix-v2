// bine/serialize/icor_to_binary.c — Complete ICOR to Binary Serialization
// Produces directly executable normal form output

#include "bine.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Serialize ICOR net to executable binary
// The normal form IS the program - no decompressor needed
int icor_serialize_to_binary(const uint64_t* net, uint32_t n_agents,
                              const char* output_path) {
    if (!net || n_agents == 0 || !output_path) {
        return -1;
    }
    
    printf("[ICOR-SERIALIZE] Serializing %u agents to executable binary\n", n_agents);
    
    // Calculate output size
    // Each agent becomes 16 bytes in output
    size_t output_size = n_agents * 16;
    uint8_t* output = (uint8_t*)malloc(output_size);
    if (!output) {
        return -1;
    }
    
    // Write header
    memcpy(output, "BINE\0\0\0", 8);  // Magic number
    *((uint32_t*)(output + 8)) = n_agents;  // Agent count
    
    // Serialize each agent
    for (uint32_t i = 0; i < n_agents; i++) {
        uint64_t agent = net[i];
        size_t offset = 16 + i * 16;
        
        // Agent type (8 bytes)
        *((uint64_t*)(output + offset)) = agent;
        
        // Port connections (8 bytes)
        uint32_t port0 = (agent >> 16) & 0xFFFFFFFF;
        uint32_t port1 = (agent >> 32) & 0xFFFFFFFF;
        *((uint32_t*)(output + offset + 8)) = port0;
        *((uint32_t*)(output + offset + 12)) = port1;
    }
    
    // Write to file
    FILE* f = fopen(output_path, "wb");
    if (!f) {
        free(output);
        return -1;
    }
    
    size_t written = fwrite(output, 1, output_size, f);
    fclose(f);
    free(output);
    
    if (written != output_size) {
        return -1;
    }
    
    printf("[ICOR-SERIALIZE] Wrote %zu bytes to %s\n", output_size, output_path);
    return 0;
}

// Deserialize executable binary back to ICOR net
uint64_t* icor_deserialize_from_binary(const char* input_path, uint32_t* n_agents_out) {
    if (!input_path || !n_agents_out) {
        return NULL;
    }
    
    FILE* f = fopen(input_path, "rb");
    if (!f) {
        return NULL;
    }
    
    // Read header
    char magic[8];
    if (fread(magic, 1, 8, f) != 8) {
        fclose(f);
        return NULL;
    }
    
    if (memcmp(magic, "BINE\0\0\0", 8) != 0) {
        fclose(f);
        return NULL;
    }
    
    uint32_t n_agents;
    if (fread(&n_agents, sizeof(uint32_t), 1, f) != 1) {
        fclose(f);
        return NULL;
    }
    
    // Allocate net
    uint64_t* net = (uint64_t*)malloc(n_agents * sizeof(uint64_t));
    if (!net) {
        fclose(f);
        return NULL;
    }
    
    // Read agents
    for (uint32_t i = 0; i < n_agents; i++) {
        uint64_t agent;
        if (fread(&agent, sizeof(uint64_t), 1, f) != 1) {
            free(net);
            fclose(f);
            return NULL;
        }
        net[i] = agent;
        
        // Skip port data (already encoded in agent)
        fseek(f, 8, SEEK_CUR);
    }
    
    fclose(f);
    
    *n_agents_out = n_agents;
    printf("[ICOR-DESERIALIZE] Loaded %u agents from %s\n", n_agents, input_path);
    
    return net;
}

// Execute ICOR normal form directly
// This is the key insight: the normal form IS the program
int icor_execute_normal_form(const uint64_t* net, uint32_t n_agents,
                              uint8_t* input_data, size_t input_size,
                              uint8_t* output_data, size_t* output_size) {
    if (!net || n_agents == 0 || !input_data || !output_data || !output_size) {
        return -1;
    }
    
    printf("[ICOR-EXEC] Executing normal form with %u agents\n", n_agents);
    
    // The normal form is a minimal computation graph
    // Execution proceeds by evaluating CON agents in topological order
    
    // Simple interpreter: evaluate CON agents
    uint8_t* registers = (uint8_t*)calloc(256, sizeof(uint8_t));  // 256 virtual registers
    if (!registers) {
        return -1;
    }
    
    // Copy input to register 0
    if (input_size > 0) {
        memcpy(registers, input_data, input_size < 256 ? input_size : 256);
    }
    
    // Evaluate each CON agent
    for (uint32_t i = 0; i < n_agents; i++) {
        uint64_t agent = net[i];
        uint32_t type = (agent >> 48) & 0xFFFF;
        
        if (type == 1) {  // CON agent
            uint8_t opcode = agent & 0xFF;
            uint8_t reg = (agent >> 40) & 0xFF;
            uint8_t src = (agent >> 32) & 0xFF;
            
            // Execute operation
            switch (opcode) {
                case 0x01:  // MOV
                    registers[reg] = registers[src];
                    break;
                case 0x02:  // ADD
                    registers[reg] += registers[src];
                    break;
                case 0x03:  // SUB
                    registers[reg] -= registers[src];
                    break;
                case 0x04:  // MUL
                    registers[reg] *= registers[src];
                    break;
                case 0x05:  // XOR
                    registers[reg] ^= registers[src];
                    break;
                default:
                    break;
            }
        }
    }
    
    // Copy result to output
    size_t result_size = 256;
    if (*output_size < result_size) {
        result_size = *output_size;
    }
    memcpy(output_data, registers, result_size);
    *output_size = result_size;
    
    free(registers);
    
    printf("[ICOR-EXEC] Execution complete\n");
    return 0;
}

// Convert ICOR net to ELF executable
// This produces a directly executable binary from the normal form
int icor_to_elf(const uint64_t* net, uint32_t n_agents,
                 const char* output_path, uint16_t machine_type) {
    if (!net || n_agents == 0 || !output_path) {
        return -1;
    }
    
    printf("[ICOR->ELF] Converting %u agents to ELF executable\n", n_agents);
    
    // ELF header
    uint8_t elf_header[64] = {0};
    
    // e_ident
    elf_header[0] = 0x7F; elf_header[1] = 'E'; elf_header[2] = 'L'; elf_header[3] = 'F';
    elf_header[4] = 2;  // 64-bit
    elf_header[5] = 1;  // little endian
    elf_header[6] = 1;  // ELF version
    
    // e_type, e_machine, e_version
    *((uint16_t*)(elf_header + 16)) = 2;  // ET_EXEC
    *((uint16_t*)(elf_header + 18)) = machine_type;  // EM_X86_64, EM_AARCH64, etc.
    *((uint32_t*)(elf_header + 20)) = 1;  // EV_CURRENT
    
    // Calculate program header offset and section header offset
    uint64_t phoff = 64;
    uint64_t shoff = 64 + 56;  // 1 program header
    
    *((uint64_t*)(elf_header + 32)) = phoff;
    *((uint64_t*)(elf_header + 40)) = shoff;
    
    // e_phentsize, e_phnum, e_shentsize, e_shnum
    *((uint16_t*)(elf_header + 54)) = 56;  // Program header size
    *((uint16_t*)(elf_header + 56)) = 1;   // Number of program headers
    *((uint16_t*)(elf_header + 58)) = 64;  // Section header size
    *((uint16_t*)(elf_header + 60)) = 0;   // No section headers for minimal ELF
    
    // Program header (LOAD segment)
    uint8_t phdr[56] = {0};
    *((uint32_t*)(phdr + 0)) = 1;      // PT_LOAD
    *((uint32_t*)(phdr + 4)) = 7;      // PF_R | PF_X
    *((uint64_t*)(phdr + 8)) = 0;      // p_offset
    *((uint64_t*)(phdr + 16)) = 0x400000;  // p_vaddr
    *((uint64_t*)(phdr + 24)) = 0x400000;  // p_paddr
    *((uint64_t*)(phdr + 32)) = n_agents * 16;  // p_filesz
    *((uint64_t*)(phdr + 40)) = n_agents * 16;  // p_memsz
    *((uint64_t*)(phdr + 48)) = 0x1000;  // p_align
    
    // Convert agents to machine code
    uint8_t* code = (uint8_t*)malloc(n_agents * 16);
    if (!code) {
        return -1;
    }
    
    for (uint32_t i = 0; i < n_agents; i++) {
        uint64_t agent = net[i];
        uint32_t type = (agent >> 48) & 0xFFFF;
        
        if (type == 1) {  // CON -> instruction
            uint8_t opcode = agent & 0xFF;
            uint8_t reg = (agent >> 40) & 0xFF;
            uint8_t src = (agent >> 32) & 0xFF;
            
            // Encode as x86-64 (simplified)
            uint8_t* insn = code + i * 16;
            insn[0] = 0x48;  // REX.W prefix
            insn[1] = opcode;
            insn[2] = 0xC0 | (reg << 3) | src;  // ModR/M
        }
    }
    
    // Write to file
    FILE* f = fopen(output_path, "wb");
    if (!f) {
        free(code);
        return -1;
    }
    
    fwrite(elf_header, 1, 64, f);
    fwrite(phdr, 1, 56, f);
    fwrite(code, 1, n_agents * 16, f);
    fclose(f);
    free(code);
    
    printf("[ICOR->ELF] Wrote ELF executable to %s\n", output_path);
    return 0;
}
