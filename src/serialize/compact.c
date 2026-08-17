// bine/serialize/elf_compact.c — ELF Binary Compaction
// Strips unnecessary sections (debug, comments, static symtab),
// removes the section header table, and truncates trailing zeros
// to produce the smallest possible executable.

#include "bine.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdbool.h>
#ifdef BINE_WINDOWS
#include <io.h>
#define read  _read
#define write _write
#define open  _open
#define close _close
typedef struct _stati64 bine_stat_t;
#define bine_fstat _fstati64
#define BINE_O_RDONLY (O_RDONLY | O_BINARY)
#define BINE_O_WRONLY_CREAT_TRUNC (O_WRONLY | O_CREAT | O_TRUNC | O_BINARY)
#else
#include <unistd.h>
typedef struct stat bine_stat_t;
#define bine_fstat fstat
#define BINE_O_RDONLY O_RDONLY
#define BINE_O_WRONLY_CREAT_TRUNC (O_WRONLY | O_CREAT | O_TRUNC)
#endif

// Section names that are safe to strip (not needed at runtime)
static const char* strippable_sections[] = {
    ".debug_info", ".debug_abbrev", ".debug_line", ".debug_str",
    ".debug_ranges", ".debug_loc", ".debug_aranges", ".debug_frame",
    ".debug_line_str", ".debug_rnglists", ".debug_loclists",
    ".debug_addr", ".debug_str_offsets", ".debug_types",
    ".note.gnu.build-id",  // Small but unnecessary for execution
    ".note.ABI-tag",
    ".note.gnu.property",
    ".comment",
    ".symtab",
    ".strtab",
    ".shstrtab",           // Only needed by section headers
    ".eh_frame_hdr",       // Exception handling (only needed for C++ exceptions / stack unwinding)
    ".eh_frame",
    ".gcc_except_table",
    NULL
};

// Sections that must NEVER be removed (needed by dynamic linker / kernel)
static const char* protected_sections[] = {
    ".dynsym", ".dynstr", ".hash", ".gnu.hash",
    ".plt", ".plt.got", ".plt.sec",
    ".got", ".got.plt",
    ".rela.dyn", ".rela.plt",
    ".dynamic",
    ".interp",
    ".text", ".rodata", ".data", ".bss",
    ".init", ".fini",
    ".init_array", ".fini_array",
    ".data.rel.ro",
    NULL
};

static bool is_strippable(const char* name) {
    if (!name) return false;
    for (int i = 0; strippable_sections[i]; i++) {
        if (strcmp(name, strippable_sections[i]) == 0) return true;
    }
    // Strip any .note.* sections
    if (strncmp(name, ".note.", 6) == 0) return true;
    // Strip any .debug* sections
    if (strncmp(name, ".debug", 6) == 0) return true;
    return false;
}

static bool section_is_loaded(const Elf64_Shdr* section,
                              const Elf64_Phdr* phdrs,
                              uint16_t n_phdrs) {
    if (!section || !phdrs || section->sh_size == 0) return false;

    uint64_t start = section->sh_offset;
    uint64_t end = start + section->sh_size;
    if (end < start) return true;

    for (uint16_t i = 0; i < n_phdrs; i++) {
        const Elf64_Phdr* phdr = &phdrs[i];
        if (phdr->p_type != PT_LOAD || phdr->p_filesz == 0) continue;

        uint64_t seg_start = phdr->p_offset;
        uint64_t seg_end = seg_start + phdr->p_filesz;
        if (seg_end < seg_start) return true;

        if (start < seg_end && end > seg_start) {
            return true;
        }
    }

    return false;
}

// Find the highest file offset used by LOAD/DYNAMIC segments
static uint64_t find_segment_ceiling(const uint8_t* buf, uint64_t file_size) {
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)buf;
    uint64_t ceiling = ehdr->e_ehsize; // At minimum, the ELF header

    // Program headers themselves
    uint64_t phdr_end = ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize;
    if (phdr_end > ceiling) ceiling = phdr_end;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr* phdr = (Elf64_Phdr*)(buf + ehdr->e_phoff + i * ehdr->e_phentsize);
        uint64_t seg_end = phdr->p_offset + phdr->p_filesz;
        if (phdr->p_filesz > 0 && seg_end > ceiling) {
            ceiling = seg_end;
        }
    }
    return ceiling;
}

// Find trailing NOP count at the end of .text
static uint64_t count_trailing_nops(const uint8_t* text, uint64_t text_size) {
    if (!text || text_size == 0) return 0;
    
    uint64_t nops = 0;
    for (uint64_t i = text_size; i > 0; i--) {
        if (text[i - 1] == 0x90 || text[i - 1] == 0x00) {
            nops++;
        } else {
            break;
        }
    }
    return nops;
}

int bine_compact_elf(const char* elf_path, const char* output_path) {
    if (!elf_path || !output_path) return -1;

    // Read input file
    int fd = open(elf_path, BINE_O_RDONLY);
    if (fd < 0) return -1;

    bine_stat_t st;
    if (bine_fstat(fd, &st) < 0) { close(fd); return -1; }

    uint64_t file_size = (uint64_t)st.st_size;
    uint8_t* buf = (uint8_t*)malloc(file_size);
    if (!buf) { close(fd); return -1; }

    if (read(fd, buf, file_size) != (ssize_t)file_size) {
        free(buf); close(fd); return -1;
    }
    close(fd);

    // Validate ELF
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)buf;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        free(buf);
        return -1;
    }

    // ========================================================================
    // Strategy 1: Find the segment ceiling (minimum safe file size)
    // ========================================================================
    uint64_t segment_ceiling = find_segment_ceiling(buf, file_size);

    // ========================================================================
    // Strategy 2: Trim trailing NOP/zero padding from .text LOAD segment
    // ========================================================================
    // Find the executable LOAD segment
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr* phdr = (Elf64_Phdr*)(buf + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (phdr->p_type == PT_LOAD && (phdr->p_flags & PF_X)) {
            uint64_t text_off = phdr->p_offset;
            uint64_t text_fsz = phdr->p_filesz;

            if (text_off + text_fsz <= file_size) {
                uint64_t trailing = count_trailing_nops(buf + text_off, text_fsz);
                // Keep at least 16 bytes of margin (alignment + potential branch targets)
                if (trailing > 16) {
                    uint64_t trim = trailing - 16;
                    // Align trim to 16 bytes
                    trim = (trim / 16) * 16;
                    if (trim > 0) {
                        phdr->p_filesz -= trim;
                        phdr->p_memsz -= trim;
                    }
                }
            }
            break; // Only first executable LOAD
        }
    }

    // ========================================================================
    // Strategy 3: Strip section header table
    // For fully-linked executables, the kernel only uses program headers.
    // ========================================================================
    // Zero out stripped sections before clearing references
    if (ehdr->e_shoff != 0 && ehdr->e_shnum > 0) {
        Elf64_Shdr* shdrs = (Elf64_Shdr*)(buf + ehdr->e_shoff);
        Elf64_Phdr* phdrs = (Elf64_Phdr*)(buf + ehdr->e_phoff);
        char* shstrtab = NULL;
        if (ehdr->e_shstrndx != SHN_UNDEF) {
            shstrtab = (char*)(buf + shdrs[ehdr->e_shstrndx].sh_offset);
        }
        for (uint16_t i = 0; i < ehdr->e_shnum; i++) {
            const char* name = shstrtab ? shstrtab + shdrs[i].sh_name : "";
            if (is_strippable(name) && !section_is_loaded(&shdrs[i], phdrs, ehdr->e_phnum)) {
                if (shdrs[i].sh_offset + shdrs[i].sh_size <= file_size) {
                    memset(buf + shdrs[i].sh_offset, 0, shdrs[i].sh_size);
                }
            }
        }
    }

    uint64_t sht_start = ehdr->e_shoff;
    uint64_t sht_size = (uint64_t)ehdr->e_shnum * ehdr->e_shentsize;
    bool sht_at_end = false;

    if (sht_start > 0 && sht_start + sht_size >= file_size - 64) {
        sht_at_end = true; // SHT is at or near end of file — safe to truncate
    }

    // Zero out the Section Header Table (SHT) itself in the buffer
    if (sht_start > segment_ceiling && sht_start + sht_size <= file_size) {
        memset(buf + sht_start, 0, sht_size);
    }

    // Zero out section header references in ELF header
    ehdr->e_shoff = 0;
    ehdr->e_shnum = 0;
    ehdr->e_shentsize = 0;
    ehdr->e_shstrndx = SHN_UNDEF;

    // ========================================================================
    // Strategy 4: Truncate the file
    // ========================================================================
    // Recalculate the segment ceiling after adjustments
    segment_ceiling = find_segment_ceiling(buf, file_size);

    uint64_t new_size = file_size;

    if (sht_at_end && sht_start > 0) {
        // SHT was at the end — try to truncate there
        new_size = sht_start;
    }

    // Scan backwards for trailing zeros
    while (new_size > segment_ceiling + 8 && buf[new_size - 1] == 0x00) {
        new_size--;
    }

    // Align to 8 bytes
    new_size = (new_size + 7) & ~(uint64_t)7;

    // Never go below segment ceiling
    if (new_size < segment_ceiling) {
        new_size = (segment_ceiling + 7) & ~(uint64_t)7;
    }

    // Safety: never exceed original size
    if (new_size > file_size) new_size = file_size;

    // ========================================================================
    // Strategy 5: Zero out NOTE segments (mark them as PT_NULL)
    // These are informational only and not needed for execution.
    // ========================================================================
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr* phdr = (Elf64_Phdr*)(buf + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (phdr->p_type == PT_NOTE) {
            // Zero the note segment content if it's within our new bounds
            if (phdr->p_offset + phdr->p_filesz <= new_size) {
                memset(buf + phdr->p_offset, 0, phdr->p_filesz);
            }
            phdr->p_type = PT_NULL; // Disable the segment
        }
        // Also disable GNU_STACK if present (it's informational)
        if (phdr->p_type == PT_GNU_STACK) {
            // Keep it but zero filesz (it has no file content anyway)
            phdr->p_filesz = 0;
            phdr->p_memsz = 0;
        }
    }

    // ========================================================================
    // Write output
    // ========================================================================
    int out_fd = open(output_path, BINE_O_WRONLY_CREAT_TRUNC, 0755);
    if (out_fd < 0) { free(buf); return -1; }

    if (write(out_fd, buf, new_size) != (ssize_t)new_size) {
        free(buf); close(out_fd); return -1;
    }

    close(out_fd);
    free(buf);
    return 0;
}
