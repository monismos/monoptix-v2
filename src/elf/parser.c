// bine/parse/elf_intrinsic.c — Stage 1: Complete ELF Self-Description Parser
// Reads all ELF structures from the binary's own bytes. Zero external data.

#include "bine.h"
#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

// ELF section header string table offset
#define SHN_UNDEF 0

// Helper: read 16-bit little-endian
static uint16_t read16_le(const uint8_t* p) {
    return p[0] | (p[1] << 8);
}

// Helper: read 32-bit little-endian
static uint32_t read32_le(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

// Helper: read 64-bit little-endian
static uint64_t read64_le(const uint8_t* p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

// Helper: read 16-bit big-endian
static uint16_t read16_be(const uint8_t* p) {
    return (p[0] << 8) | p[1];
}

// Helper: read 32-bit big-endian
static uint32_t read32_be(const uint8_t* p) {
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

// Helper: read 64-bit big-endian
static uint64_t read64_be(const uint8_t* p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) | ((uint64_t)p[2] << 40) |
           ((uint64_t)p[3] << 32) | ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) | ((uint64_t)p[7]);
}

// Check ELF endianness
static bool is_little_endian(const uint8_t* e_ident) {
    return e_ident[EI_DATA] == ELFDATA2LSB;
}

// Find a section by name in the section header string table
static const Elf64_Shdr* find_section(const char* name, const Elf64_Shdr* shdrs,
                                      uint32_t n_shdrs, const char* shstrtab) {
    for (uint32_t i = 0; i < n_shdrs; i++) {
        if (shdrs[i].sh_name != 0) {
            const char* sname = shstrtab + shdrs[i].sh_name;
            if (strcmp(sname, name) == 0) {
                return &shdrs[i];
            }
        }
    }
    return NULL;
}

// Extract symbols from .symtab or .dynsym
static int extract_symbols(const uint8_t* file_data, size_t file_size,
                          const Elf64_Shdr* symtab_shdr, const char* strtab,
                          Symbol** symbols_out, uint32_t* n_symbols_out) {
    if (!symtab_shdr || symtab_shdr->sh_type != SHT_SYMTAB && 
        symtab_shdr->sh_type != SHT_DYNSYM) {
        *symbols_out = NULL;
        *n_symbols_out = 0;
        return 0;
    }
    
    uint64_t symtab_offset = symtab_shdr->sh_offset;
    uint64_t symtab_size = symtab_shdr->sh_size;
    uint32_t symtab_entsize = symtab_shdr->sh_entsize;
    
    if (symtab_entsize == 0) symtab_entsize = sizeof(Elf64_Sym);
    uint32_t n_syms = symtab_size / symtab_entsize;
    
    Symbol* symbols = (Symbol*)calloc(n_syms, sizeof(Symbol));
    if (!symbols) return -1;
    
    uint32_t n_valid = 0;
    for (uint32_t i = 0; i < n_syms; i++) {
        uint64_t sym_offset = symtab_offset + i * symtab_entsize;
        if (sym_offset + sizeof(Elf64_Sym) > file_size) break;
        
        const Elf64_Sym* sym = (const Elf64_Sym*)(file_data + sym_offset);
        
        // Skip undefined symbols
        if (sym->st_shndx == SHN_UNDEF) continue;
        
        // Only include functions, objects, and NOTYPE (for completeness)
        uint8_t type = ELF64_ST_TYPE(sym->st_info);
        if (type != STT_FUNC && type != STT_OBJECT && type != STT_NOTYPE) continue;
        
        Symbol* s = &symbols[n_valid++];
        s->name = (sym->st_name != 0) ? (strtab + sym->st_name) : "";
        s->address = sym->st_value;
        s->size = sym->st_size;
        s->type = type;
        s->binding = ELF64_ST_BIND(sym->st_info);
    }
    
    // Reallocate to exact size
    if (n_valid > 0 && n_valid < n_syms) {
        Symbol* tmp = (Symbol*)realloc(symbols, n_valid * sizeof(Symbol));
        if (tmp) symbols = tmp;
    }
    
    *symbols_out = symbols;
    *n_symbols_out = n_valid;
    return 0;
}

// Extract relocations from .rela.* sections
static int extract_relocations(const uint8_t* file_data, size_t file_size,
                              const Elf64_Shdr* shdrs, uint32_t n_shdrs,
                              const Elf64_Shdr* symtab_shdr, const char* strtab,
                              Reloc** relocs_out, uint32_t* n_relocs_out) {
    uint32_t total_relocs = 0;
    
    // First pass: count relocations
    for (uint32_t i = 0; i < n_shdrs; i++) {
        if (shdrs[i].sh_type == SHT_RELA) {
            total_relocs += shdrs[i].sh_size / sizeof(Elf64_Rela);
        }
    }
    
    if (total_relocs == 0) {
        *relocs_out = NULL;
        *n_relocs_out = 0;
        return 0;
    }
    
    Reloc* relocs = (Reloc*)calloc(total_relocs, sizeof(Reloc));
    if (!relocs) return -1;
    
    uint32_t n_valid = 0;
    for (uint32_t i = 0; i < n_shdrs; i++) {
        if (shdrs[i].sh_type == SHT_RELA) {
            uint64_t rela_offset = shdrs[i].sh_offset;
            uint64_t rela_size = shdrs[i].sh_size;
            uint32_t n_entries = rela_size / sizeof(Elf64_Rela);
            
            for (uint32_t j = 0; j < n_entries; j++) {
                uint64_t entry_offset = rela_offset + j * sizeof(Elf64_Rela);
                if (entry_offset + sizeof(Elf64_Rela) > file_size) break;
                
                const Elf64_Rela* rela = (const Elf64_Rela*)(file_data + entry_offset);
                
                Reloc* r = &relocs[n_valid++];
                r->offset = rela->r_offset;
                r->symbol_idx = ELF64_R_SYM(rela->r_info);
                r->addend = rela->r_addend;
                r->type = ELF64_R_TYPE(rela->r_info);
            }
        }
    }
    
    *relocs_out = relocs;
    *n_relocs_out = n_valid;
    return 0;
}

// Extract NEEDED libraries from .dynamic section
static int extract_needed_libs(const uint8_t* file_data, size_t file_size,
                               const Elf64_Shdr* dynamic_shdr, const char* strtab,
                               char*** needed_out, uint32_t* n_needed_out) {
    if (!dynamic_shdr || dynamic_shdr->sh_type != SHT_DYNAMIC) {
        *needed_out = NULL;
        *n_needed_out = 0;
        return 0;
    }
    
    uint64_t dynamic_offset = dynamic_shdr->sh_offset;
    uint64_t dynamic_size = dynamic_shdr->sh_size;
    uint32_t n_entries = dynamic_size / sizeof(Elf64_Dyn);
    
    // First pass: count NEEDED entries
    uint32_t n_needed = 0;
    for (uint32_t i = 0; i < n_entries; i++) {
        uint64_t entry_offset = dynamic_offset + i * sizeof(Elf64_Dyn);
        if (entry_offset + sizeof(Elf64_Dyn) > file_size) break;
        
        const Elf64_Dyn* dyn = (const Elf64_Dyn*)(file_data + entry_offset);
        if (dyn->d_tag == DT_NEEDED) {
            n_needed++;
        }
    }
    
    if (n_needed == 0) {
        *needed_out = NULL;
        *n_needed_out = 0;
        return 0;
    }
    
    char** needed = (char**)calloc(n_needed, sizeof(char*));
    if (!needed) return -1;
    
    uint32_t idx = 0;
    for (uint32_t i = 0; i < n_entries; i++) {
        uint64_t entry_offset = dynamic_offset + i * sizeof(Elf64_Dyn);
        if (entry_offset + sizeof(Elf64_Dyn) > file_size) break;
        
        const Elf64_Dyn* dyn = (const Elf64_Dyn*)(file_data + entry_offset);
        if (dyn->d_tag == DT_NEEDED && dyn->d_un.d_val != 0) {
            needed[idx++] = strdup(strtab + dyn->d_un.d_val);
        }
    }
    
    *needed_out = needed;
    *n_needed_out = n_needed;
    return 0;
}

// Extract build ID from .note.gnu.build-id
static int extract_build_id(const uint8_t* file_data, size_t file_size,
                            const Elf64_Shdr* shdrs, uint32_t n_shdrs,
                            uint8_t build_id[20], bool* has_build_id) {
    const Elf64_Shdr* note_shdr = find_section(".note.gnu.build-id", shdrs, n_shdrs, "");
    if (!note_shdr) {
        memset(build_id, 0, 20);
        *has_build_id = false;
        return 0;
    }
    
    uint64_t note_offset = note_shdr->sh_offset;
    uint64_t note_size = note_shdr->sh_size;
    
    if (note_offset + note_size > file_size) {
        memset(build_id, 0, 20);
        *has_build_id = false;
        return 0;
    }
    
    // Parse note header: namesz (4), descsz (4), type (4)
    if (note_size < 12) {
        memset(build_id, 0, 20);
        *has_build_id = false;
        return 0;
    }
    
    uint32_t namesz = read32_le(file_data + note_offset);
    uint32_t descsz = read32_le(file_data + note_offset + 4);
    // uint32_t type = read32_le(file_data + note_offset + 8);
    
    uint64_t name_offset = note_offset + 12;
    uint64_t desc_offset = name_offset + ((namesz + 3) & ~3); // aligned
    
    if (descsz >= 20 && desc_offset + 20 <= file_size) {
        memcpy(build_id, file_data + desc_offset, 20);
        *has_build_id = true;
    } else {
        memset(build_id, 0, 20);
        *has_build_id = false;
    }
    
    return 0;
}

// Extract compiler string from .comment section
static int extract_compiler(const uint8_t* file_data, size_t file_size,
                           const Elf64_Shdr* shdrs, uint32_t n_shdrs,
                           char** compiler_out) {
    const Elf64_Shdr* comment_shdr = find_section(".comment", shdrs, n_shdrs, "");
    if (!comment_shdr || comment_shdr->sh_size == 0) {
        *compiler_out = NULL;
        return 0;
    }
    
    uint64_t comment_offset = comment_shdr->sh_offset;
    uint64_t comment_size = comment_shdr->sh_size;
    
    if (comment_offset + comment_size > file_size) {
        *compiler_out = NULL;
        return 0;
    }
    
    // Ensure null-terminated
    char* compiler = (char*)malloc(comment_size + 1);
    if (!compiler) return -1;
    
    memcpy(compiler, file_data + comment_offset, comment_size);
    compiler[comment_size] = '\0';
    
    *compiler_out = compiler;
    return 0;
}

// Main extraction function
BinaryIntrinsic* bine_extract_intrinsic(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }
    
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }
    
    size_t file_size = st.st_size;
    if (file_size < sizeof(Elf64_Ehdr)) {
        close(fd);
        return NULL;
    }
    
    // Memory-map the file
    uint8_t* file_data = (uint8_t*)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (file_data == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    
    close(fd);
    
    // Verify ELF magic
    if (memcmp(file_data, ELFMAG, SELFMAG) != 0) {
        munmap(file_data, file_size);
        return NULL;
    }
    
    bool le = is_little_endian(file_data);
    uint8_t elf_class = file_data[EI_CLASS];
    
    if (elf_class != ELFCLASS64) {
        munmap(file_data, file_size);
        return NULL;
    }
    
    // Parse ELF header
    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)file_data;
    
    BinaryIntrinsic* bin = (BinaryIntrinsic*)calloc(1, sizeof(BinaryIntrinsic));
    if (!bin) {
        munmap(file_data, file_size);
        return NULL;
    }
    
    // Store path
    bin->path = strdup(path);
    
    // Extract ELF header fields
    bin->e_machine = le ? read16_le((uint8_t*)&ehdr->e_machine) : read16_be((uint8_t*)&ehdr->e_machine);
    bin->e_entry = le ? read64_le((uint8_t*)&ehdr->e_entry) : read64_be((uint8_t*)&ehdr->e_entry);
    bin->e_class = elf_class;
    
    // Parse program headers
    uint64_t phoff = le ? read64_le((uint8_t*)&ehdr->e_phoff) : read64_be((uint8_t*)&ehdr->e_phoff);
    uint16_t phnum = le ? read16_le((uint8_t*)&ehdr->e_phnum) : read16_be((uint8_t*)&ehdr->e_phnum);
    uint16_t phentsize = le ? read16_le((uint8_t*)&ehdr->e_phentsize) : read16_be((uint8_t*)&ehdr->e_phentsize);
    
    // Find executable LOAD segment
    for (uint16_t i = 0; i < phnum; i++) {
        uint64_t ph_offset = phoff + i * phentsize;
        if (ph_offset + sizeof(Elf64_Phdr) > file_size) break;
        
        const Elf64_Phdr* phdr = (const Elf64_Phdr*)(file_data + ph_offset);
        uint32_t p_type = le ? read32_le((uint8_t*)&phdr->p_type) : read32_be((uint8_t*)&phdr->p_type);
        
        if (p_type == PT_LOAD) {
            uint32_t p_flags = le ? read32_le((uint8_t*)&phdr->p_flags) : read32_be((uint8_t*)&phdr->p_flags);
            if (p_flags & PF_X) {
                bin->load_vaddr = le ? read64_le((uint8_t*)&phdr->p_vaddr) : read64_be((uint8_t*)&phdr->p_vaddr);
                bin->load_offset = le ? read64_le((uint8_t*)&phdr->p_offset) : read64_be((uint8_t*)&phdr->p_offset);
                bin->load_size = le ? read64_le((uint8_t*)&phdr->p_filesz) : read64_be((uint8_t*)&phdr->p_filesz);
                break;
            }
        }
    }
    
    // Parse section headers
    uint64_t shoff = le ? read64_le((uint8_t*)&ehdr->e_shoff) : read64_be((uint8_t*)&ehdr->e_shoff);
    uint16_t shnum = le ? read16_le((uint8_t*)&ehdr->e_shnum) : read16_be((uint8_t*)&ehdr->e_shnum);
    uint16_t shentsize = le ? read16_le((uint8_t*)&ehdr->e_shentsize) : read16_be((uint8_t*)&ehdr->e_shentsize);
    uint16_t shstrndx = le ? read16_le((uint8_t*)&ehdr->e_shstrndx) : read16_be((uint8_t*)&ehdr->e_shstrndx);
    
    // Read section header string table
    const char* shstrtab = "";
    if (shstrndx < shnum) {
        uint64_t shstrtab_offset = shoff + shstrndx * shentsize;
        if (shstrtab_offset + sizeof(Elf64_Shdr) <= file_size) {
            const Elf64_Shdr* shstr_shdr = (const Elf64_Shdr*)(file_data + shstrtab_offset);
            uint64_t strtab_offset = le ? read64_le((uint8_t*)&shstr_shdr->sh_offset) : read64_be((uint8_t*)&shstr_shdr->sh_offset);
            uint64_t strtab_size = le ? read64_le((uint8_t*)&shstr_shdr->sh_size) : read64_be((uint8_t*)&shstr_shdr->sh_size);
            if (strtab_offset + strtab_size <= file_size) {
                shstrtab = (const char*)(file_data + strtab_offset);
            }
        }
    }
    
    // Allocate and read section headers
    Elf64_Shdr* shdrs = (Elf64_Shdr*)malloc(shnum * sizeof(Elf64_Shdr));
    if (!shdrs) {
        munmap(file_data, file_size);
        bine_free_intrinsic(bin);
        return NULL;
    }
    
    for (uint16_t i = 0; i < shnum; i++) {
        uint64_t sh_offset = shoff + i * shentsize;
        if (sh_offset + sizeof(Elf64_Shdr) > file_size) break;
        memcpy(&shdrs[i], file_data + sh_offset, sizeof(Elf64_Shdr));
        // Convert to host byte order if needed
        if (!le) {
            shdrs[i].sh_name = read32_be((uint8_t*)&shdrs[i].sh_name);
            shdrs[i].sh_type = read32_be((uint8_t*)&shdrs[i].sh_type);
            shdrs[i].sh_flags = read64_be((uint8_t*)&shdrs[i].sh_flags);
            shdrs[i].sh_addr = read64_be((uint8_t*)&shdrs[i].sh_addr);
            shdrs[i].sh_offset = read64_be((uint8_t*)&shdrs[i].sh_offset);
            shdrs[i].sh_size = read64_be((uint8_t*)&shdrs[i].sh_size);
        }
    }
    
    // Find and extract key sections
    const Elf64_Shdr* text_shdr = find_section(".text", shdrs, shnum, shstrtab);
    const Elf64_Shdr* rodata_shdr = find_section(".rodata", shdrs, shnum, shstrtab);
    const Elf64_Shdr* data_shdr = find_section(".data", shdrs, shnum, shstrtab);
    const Elf64_Shdr* symtab_shdr = find_section(".symtab", shdrs, shnum, shstrtab);
    const Elf64_Shdr* dynsym_shdr = find_section(".dynsym", shdrs, shnum, shstrtab);
    const Elf64_Shdr* strtab_shdr = find_section(".strtab", shdrs, shnum, shstrtab);
    const Elf64_Shdr* dynstr_shdr = find_section(".dynstr", shdrs, shnum, shstrtab);
    const Elf64_Shdr* dynamic_shdr = find_section(".dynamic", shdrs, shnum, shstrtab);
    
    // Use .dynsym if .symtab not available (stripped binaries)
    if (!symtab_shdr) symtab_shdr = dynsym_shdr;
    if (!strtab_shdr) strtab_shdr = dynstr_shdr;
    
    // Extract .text
    if (text_shdr) {
        uint64_t text_offset = text_shdr->sh_offset;
        uint64_t text_size = text_shdr->sh_size;
        if (text_offset + text_size <= file_size) {
            bin->text_bytes = (uint8_t*)malloc(text_size);
            if (bin->text_bytes) {
                memcpy(bin->text_bytes, file_data + text_offset, text_size);
                bin->text_size = text_size;
                bin->text_vaddr = text_shdr->sh_addr;
            }
        }
    }
    
    // Extract .rodata
    if (rodata_shdr) {
        uint64_t rodata_offset = rodata_shdr->sh_offset;
        uint64_t rodata_size = rodata_shdr->sh_size;
        if (rodata_offset + rodata_size <= file_size) {
            bin->rodata_bytes = (uint8_t*)malloc(rodata_size);
            if (bin->rodata_bytes) {
                memcpy(bin->rodata_bytes, file_data + rodata_offset, rodata_size);
                bin->rodata_size = rodata_size;
            }
        }
    }
    
    // Extract .data
    if (data_shdr) {
        uint64_t data_offset = data_shdr->sh_offset;
        uint64_t data_size = data_shdr->sh_size;
        if (data_offset + data_size <= file_size) {
            bin->data_bytes = (uint8_t*)malloc(data_size);
            if (bin->data_bytes) {
                memcpy(bin->data_bytes, file_data + data_offset, data_size);
                bin->data_size = data_size;
            }
        }
    }
    
    // Extract string table for symbols
    const char* strtab = "";
    if (strtab_shdr) {
        uint64_t strtab_offset = strtab_shdr->sh_offset;
        uint64_t strtab_size = strtab_shdr->sh_size;
        if (strtab_offset + strtab_size <= file_size) {
            strtab = (const char*)(file_data + strtab_offset);
        }
    }
    
    // Extract symbols
    if (symtab_shdr) {
        extract_symbols(file_data, file_size, symtab_shdr, strtab,
                       &bin->symbols, &bin->n_symbols);
    }
    
    // Extract relocations
    extract_relocations(file_data, file_size, shdrs, shnum, symtab_shdr, strtab,
                      &bin->relocs, &bin->n_relocs);
    
    // Extract dynamic string table for NEEDED libraries
    const char* dynstr = "";
    if (dynstr_shdr) {
        uint64_t dynstr_offset = dynstr_shdr->sh_offset;
        uint64_t dynstr_size = dynstr_shdr->sh_size;
        if (dynstr_offset + dynstr_size <= file_size) {
            dynstr = (const char*)(file_data + dynstr_offset);
        }
    }
    
    // Extract NEEDED libraries
    if (dynamic_shdr) {
        extract_needed_libs(file_data, file_size, dynamic_shdr, dynstr,
                           &bin->needed_libs, &bin->n_needed);
    }
    
    // Extract build ID
    extract_build_id(file_data, file_size, shdrs, shnum, bin->build_id, &bin->has_build_id);
    
    // Extract compiler string
    extract_compiler(file_data, file_size, shdrs, shnum, &bin->compiler_str);
    
    // Calculate fingerprint from inode metadata
    bin->fingerprint = bine_get_fingerprint(path);
    
    free(shdrs);
    munmap(file_data, file_size);

    return bin;
}

void bine_free_intrinsic(BinaryIntrinsic* bin) {
    if (!bin) return;
    
    free(bin->text_bytes);
    free(bin->rodata_bytes);
    free(bin->data_bytes);
    free(bin->symbols);
    free(bin->relocs);
    if (bin->needed_libs) {
        for (uint32_t i = 0; i < bin->n_needed; i++) {
            free(bin->needed_libs[i]);
        }
        free(bin->needed_libs);
    }
    free(bin->compiler_str);
    free(bin->path);
    free(bin);
}
