// bine/oracle/load_address.c — Oracle 1: Load address from ELF LOAD segment
// Reads p_vaddr from the binary's own program headers.

#include "bine.h"
#include <elf.h>
#include <stdint.h>

uint64_t bine_get_load_address(const BinaryIntrinsic* bin) {
    // The load address is already extracted in Stage 1
    // For non-PIE binaries, this is the final absolute address
    // For PIE binaries, we use 0 for analysis (offset-independent)
    return bin->load_vaddr;
}
// bine/oracle/mtime.c — Oracle 2: Fingerprint from inode metadata
// Uses file mtime, inode, and size as a cache key for O(1) amortized optimization.

#include "bine.h"
#include <sys/stat.h>
#include <unistd.h>

// FNV-1a 64-bit hash implementation
uint64_t fnv1a_64(const uint8_t* data, uint64_t len) {
    uint64_t hash = 14695981039346656037ULL; // FNV offset basis
    for (uint64_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 1099511628211ULL; // FNV prime
    }
    return hash;
}

uint64_t bine_get_fingerprint(const char* path) {
    struct stat st;
    if (stat(path, &st) < 0) {
        return 0;
    }
    
    // Fingerprint = hash of (inode, mtime_sec, mtime_nsec, size)
    uint64_t hash = fnv1a_64((uint8_t*)&st.st_ino, sizeof(st.st_ino));
    hash ^= fnv1a_64((uint8_t*)&st.st_mtim.tv_sec, sizeof(st.st_mtim.tv_sec));
    hash ^= fnv1a_64((uint8_t*)&st.st_mtim.tv_nsec, sizeof(st.st_mtim.tv_nsec));
    hash ^= fnv1a_64((uint8_t*)&st.st_size, sizeof(st.st_size));
    
    return hash;
}

// bine/oracle/path.c — Oracle 3: File path parsing + sibling detection
// Uses the file path to identify package namespace and locate sibling binaries.

#include "bine.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libgen.h>
#include <dirent.h>

// Extract directory from path (returns newly allocated string)
char* bine_get_directory(const char* path) {
    char* path_copy = strdup(path);
    if (!path_copy) return NULL;
    
    char* dir = dirname(path_copy);
    char* result = strdup(dir);
    
    free(path_copy);
    return result;
}

// Extract binary name from path (returns newly allocated string)
char* bine_get_basename(const char* path) {
    char* path_copy = strdup(path);
    if (!path_copy) return NULL;
    
    char* base = basename(path_copy);
    char* result = strdup(base);
    
    free(path_copy);
    return result;
}

// Find sibling binaries in the same directory
char** bine_find_siblings(const char* path, uint32_t* n_siblings) {
    char* dir = bine_get_directory(path);
    if (!dir) {
        *n_siblings = 0;
        return NULL;
    }
    
    DIR* d = opendir(dir);
    if (!d) {
        free(dir);
        *n_siblings = 0;
        return NULL;
    }
    
    // Count siblings first
    uint32_t count = 0;
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_type == DT_REG || entry->d_type == DT_LNK) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                count++;
            }
        }
    }
    
    rewinddir(d);
    
    if (count == 0) {
        closedir(d);
        free(dir);
        *n_siblings = 0;
        return NULL;
    }
    
    // Allocate array
    char** siblings = (char**)calloc(count, sizeof(char*));
    if (!siblings) {
        closedir(d);
        free(dir);
        *n_siblings = 0;
        return NULL;
    }
    
    // Fill array
    uint32_t idx = 0;
    while ((entry = readdir(d)) != NULL && idx < count) {
        if (entry->d_type == DT_REG || entry->d_type == DT_LNK) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                char full_path[PATH_MAX];
                snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name);
                siblings[idx++] = strdup(full_path);
            }
        }
    }
    
    closedir(d);
    free(dir);
    
    *n_siblings = idx;
    return siblings;
}

void bine_free_siblings(char** siblings, uint32_t n_siblings) {
    if (!siblings) return;
    for (uint32_t i = 0; i < n_siblings; i++) {
        free(siblings[i]);
    }
    free(siblings);
}

// bine/oracle/proc_context.c — Oracle 4: Process context from /proc/self
// Reads memory mappings, file descriptors, and resource usage.

#include "bine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

typedef struct {
    uint64_t start_addr;
    uint64_t end_addr;
    uint32_t permissions;
    char     path[256];
} MemoryMapping;

MemoryMapping* bine_get_memory_maps(uint32_t* n_maps) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) {
        *n_maps = 0;
        return NULL;
    }
    
    // Count lines first
    uint32_t count = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        count++;
    }
    
    fseek(f, 0, SEEK_SET);
    
    MemoryMapping* maps = (MemoryMapping*)calloc(count, sizeof(MemoryMapping));
    if (!maps) {
        fclose(f);
        *n_maps = 0;
        return NULL;
    }
    
    uint32_t idx = 0;
    while (fgets(line, sizeof(line), f) && idx < count) {
        uint64_t start, end;
        char perms[5];
        char path[256];
        
        if (sscanf(line, "%lx-%lx %4s %*x %*x:%*x %*u %255s",
                   &start, &end, perms, path) == 4) {
            maps[idx].start_addr = start;
            maps[idx].end_addr = end;
            
            // Parse permissions
            maps[idx].permissions = 0;
            if (perms[0] == 'r') maps[idx].permissions |= 0x1;
            if (perms[1] == 'w') maps[idx].permissions |= 0x2;
            if (perms[2] == 'x') maps[idx].permissions |= 0x4;
            
            strncpy(maps[idx].path, path, sizeof(maps[idx].path) - 1);
            maps[idx].path[sizeof(maps[idx].path) - 1] = '\0';
            idx++;
        }
    }
    
    fclose(f);
    *n_maps = idx;
    return maps;
}

void bine_free_memory_maps(MemoryMapping* maps) {
    free(maps);
}

// Get open file descriptors
char** bine_get_open_fds(uint32_t* n_fds) {
    char path[256];
    uint32_t count = 0;
    
    // Count open FDs
    for (int i = 0; i < 1024; i++) {
        snprintf(path, sizeof(path), "/proc/self/fd/%d", i);
        if (access(path, F_OK) == 0) {
            count++;
        }
    }
    
    if (count == 0) {
        *n_fds = 0;
        return NULL;
    }
    
    char** fds = (char**)calloc(count, sizeof(char*));
    if (!fds) {
        *n_fds = 0;
        return NULL;
    }
    
    uint32_t idx = 0;
    for (int i = 0; i < 1024 && idx < count; i++) {
        snprintf(path, sizeof(path), "/proc/self/fd/%d", i);
        if (access(path, F_OK) == 0) {
            fds[idx++] = strdup(path);
        }
    }
    
    *n_fds = idx;
    return fds;
}

void bine_free_fds(char** fds, uint32_t n_fds) {
    if (!fds) return;
    for (uint32_t i = 0; i < n_fds; i++) {
        free(fds[i]);
    }
    free(fds);
}

// Get resource usage (for priority bias)
void bine_get_resource_usage(uint64_t* utime_usec, uint64_t* stime_usec) {
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) {
        *utime_usec = 0;
        *stime_usec = 0;
        return;
    }
    
    // /proc/self/stat format: utime (14th field), stime (15th field)
    char line[1024];
    if (fgets(line, sizeof(line), f)) {
        char* token = line;
        for (int i = 0; i < 13; i++) {
            token = strchr(token, ' ');
            if (!token) break;
            token++;
        }
        if (token) {
            *utime_usec = strtoull(token, &token, 10);
            if (token) {
                *stime_usec = strtoull(token, NULL, 10);
            }
        }
    }
    
    fclose(f);
}

// bine/oracle/call_trace.c — Oracle 5: Call trace from PLT/GOT structure
// Parses PLT, GOT, and .rela.plt to extract the complete call graph.

#include "bine.h"
#include <stdlib.h>
#include <string.h>
#include <elf.h>

typedef struct {
    uint64_t plt_offset;
    uint64_t got_offset;
    char*   symbol_name;
    uint32_t call_frequency; // Estimated from PLT stub count
} PLTEntry;

typedef struct {
    uint64_t caller_addr;
    uint64_t callee_addr;
    uint32_t call_count;
} CallEdge;

// Extract PLT entries from .plt and .rela.plt sections
PLTEntry* bine_extract_plt(const BinaryIntrinsic* bin, uint32_t* n_plt) {
    if (!bin || !bin->relocs || bin->n_relocs == 0) {
        *n_plt = 0;
        return NULL;
    }
    
    // Count .rela.plt entries
    uint32_t plt_count = 0;
    for (uint32_t i = 0; i < bin->n_relocs; i++) {
        if (bin->relocs[i].type == R_X86_64_JUMP_SLOT) {
            plt_count++;
        }
    }
    
    if (plt_count == 0) {
        *n_plt = 0;
        return NULL;
    }
    
    PLTEntry* plt_entries = (PLTEntry*)calloc(plt_count, sizeof(PLTEntry));
    if (!plt_entries) {
        *n_plt = 0;
        return NULL;
    }
    
    uint32_t idx = 0;
    for (uint32_t i = 0; i < bin->n_relocs; i++) {
        if (bin->relocs[i].type == R_X86_64_JUMP_SLOT) {
            plt_entries[idx].plt_offset = bin->relocs[i].offset;
            plt_entries[idx].got_offset = bin->relocs[i].addend;
            // Look up symbol name from symbol_idx
            if (bin->relocs[i].symbol_idx < bin->n_symbols && bin->symbols) {
                plt_entries[idx].symbol_name = strdup(bin->symbols[bin->relocs[i].symbol_idx].name);
            } else {
                plt_entries[idx].symbol_name = NULL;
            }
            plt_entries[idx].call_frequency = 1; // Default frequency
            idx++;
        }
    }
    
    *n_plt = plt_count;
    return plt_entries;
}

// Extract GOT entries
uint64_t* bine_extract_got(const BinaryIntrinsic* bin, uint32_t* n_got) {
    if (!bin || !bin->relocs || bin->n_relocs == 0) {
        *n_got = 0;
        return NULL;
    }
    
    // Count GOT-related relocations
    uint32_t got_count = 0;
    for (uint32_t i = 0; i < bin->n_relocs; i++) {
        if (bin->relocs[i].type == R_X86_64_GLOB_DAT ||
            bin->relocs[i].type == R_X86_64_IRELATIVE) {
            got_count++;
        }
    }
    
    if (got_count == 0) {
        *n_got = 0;
        return NULL;
    }
    
    uint64_t* got_entries = (uint64_t*)calloc(got_count, sizeof(uint64_t));
    if (!got_entries) {
        *n_got = 0;
        return NULL;
    }
    
    uint32_t idx = 0;
    for (uint32_t i = 0; i < bin->n_relocs; i++) {
        if (bin->relocs[i].type == R_X86_64_GLOB_DAT ||
            bin->relocs[i].type == R_X86_64_IRELATIVE) {
            got_entries[idx++] = bin->relocs[i].offset;
        }
    }
    
    *n_got = got_count;
    return got_entries;
}

// Build call graph from symbol table and .text section
CallEdge* bine_build_call_graph(const BinaryIntrinsic* bin, uint32_t* n_edges) {
    if (!bin || !bin->symbols || bin->n_symbols == 0) {
        *n_edges = 0;
        return NULL;
    }
    
    // Decode instructions to find CALL instructions
    uint32_t n_insns = 0;
    Instruction* insns = decode_instructions(bin->text_bytes, bin->text_size, &n_insns);
    if (!insns || n_insns == 0) {
        *n_edges = 0;
        return NULL;
    }
    
    // Count CALL instructions
    uint32_t call_count = 0;
    for (uint32_t i = 0; i < n_insns; i++) {
        if (insns[i].is_call) {
            call_count++;
        }
    }
    
    if (call_count == 0) {
        free_instructions(insns);
        *n_edges = 0;
        return NULL;
    }
    
    CallEdge* edges = (CallEdge*)calloc(call_count, sizeof(CallEdge));
    if (!edges) {
        free_instructions(insns);
        *n_edges = 0;
        return NULL;
    }
    
    uint32_t idx = 0;
    for (uint32_t i = 0; i < n_insns; i++) {
        if (insns[i].is_call) {
            edges[idx].caller_addr = insns[i].offset;
            edges[idx].callee_addr = insns[i].target;
            edges[idx].call_count = 1;
            idx++;
        }
    }
    
    free_instructions(insns);
    *n_edges = call_count;
    return edges;
}

// Estimate call frequency from symbol size (proxy for how often a function is called)
uint32_t bine_estimate_call_frequency(const Symbol* sym, const BinaryIntrinsic* bin) {
    // Larger functions called from more sites = higher frequency
    // Use symbol size as a simple proxy
    return sym->size;
}

void bine_free_plt(PLTEntry* plt, uint32_t n_plt) {
    if (!plt) return;
    for (uint32_t i = 0; i < n_plt; i++) {
        free(plt[i].symbol_name);
    }
    free(plt);
}

void bine_free_call_graph(CallEdge* edges) {
    free(edges);
}
