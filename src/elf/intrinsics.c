// bine/oracle/intrinsics.c — Self-description oracles.
// Load address, fingerprint, path parsing, sibling detection, process
// context and call-graph extraction.  All platform differences are
// isolated here; Linux-specific introspection (procfs) degrades to
// empty results on other platforms rather than failing.

#include "bine.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(BINE_WINDOWS)
#include <windows.h>
#include <sys/stat.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

// ============================================================================
// Oracle 1: Load address from ELF LOAD segment
// ============================================================================

uint64_t bine_get_load_address(const BinaryIntrinsic* bin) {
    // The load address is already extracted in Stage 1.
    // For non-PIE binaries this is the final absolute address;
    // for PIE binaries we use 0 for analysis (offset-independent).
    return bin->load_vaddr;
}

// ============================================================================
// Oracle 2: Fingerprint from file metadata
// ============================================================================

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
#if defined(BINE_WINDOWS)
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
#endif

    // Fingerprint = hash of (size, mtime). Inode identity differs across
    // platforms and filesystems; size+mtime is the portable, stable key.
    uint64_t hash = fnv1a_64((uint8_t*)&st.st_size, sizeof(st.st_size));
    hash ^= fnv1a_64((uint8_t*)&st.st_mtime, sizeof(st.st_mtime));
    return hash;
}

// ============================================================================
// Oracle 3: File path parsing + sibling detection
// ============================================================================

// Extract directory from path (returns newly allocated string)
char* bine_get_directory(const char* path) {
    if (!path) return NULL;
    const char* last_sep = strrchr(path, '/');
#if defined(BINE_WINDOWS)
    const char* back = strrchr(path, '\\');
    if (!last_sep || (back && back > last_sep)) last_sep = back;
#endif
    if (!last_sep) return strdup(".");
    size_t n = (size_t)(last_sep - path);
    if (n == 0) return strdup("/");
    char* result = (char*)malloc(n + 1);
    if (!result) return NULL;
    memcpy(result, path, n);
    result[n] = 0;
    return result;
}

// Extract binary name from path (returns newly allocated string)
char* bine_get_basename(const char* path) {
    if (!path) return NULL;
    const char* last_sep = strrchr(path, '/');
#if defined(BINE_WINDOWS)
    const char* back = strrchr(path, '\\');
    if (!last_sep || (back && back > last_sep)) last_sep = back;
#endif
    const char* base = last_sep ? last_sep + 1 : path;
    return strdup(base);
}

// Find sibling binaries in the same directory
char** bine_find_siblings(const char* path, uint32_t* n_siblings) {
    *n_siblings = 0;
    char* dir = bine_get_directory(path);
    if (!dir) return NULL;

#if defined(BINE_WINDOWS)
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { free(dir); return NULL; }

    uint32_t cap = 16, idx = 0;
    char** siblings = (char**)calloc(cap, sizeof(char*));
    if (!siblings) { FindClose(h); free(dir); return NULL; }

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (idx == cap) {
            cap *= 2;
            char** nb = (char**)realloc(siblings, cap * sizeof(char*));
            if (!nb) break;
            siblings = nb;
        }
        char full[MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", dir, fd.cFileName);
        siblings[idx++] = strdup(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    free(dir);
    *n_siblings = idx;
    return siblings;
#else
    DIR* d = opendir(dir);
    if (!d) { free(dir); return NULL; }

    uint32_t cap = 16, idx = 0;
    char** siblings = (char**)calloc(cap, sizeof(char*));
    if (!siblings) { closedir(d); free(dir); return NULL; }

    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (entry->d_type == DT_DIR) continue;
        if (idx == cap) {
            cap *= 2;
            char** nb = (char**)realloc(siblings, cap * sizeof(char*));
            if (!nb) break;
            siblings = nb;
        }
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name);
        siblings[idx++] = strdup(full);
    }
    closedir(d);
    free(dir);
    *n_siblings = idx;
    return siblings;
#endif
}

void bine_free_siblings(char** siblings, uint32_t n_siblings) {
    if (!siblings) return;
    for (uint32_t i = 0; i < n_siblings; i++) {
        free(siblings[i]);
    }
    free(siblings);
}

// ============================================================================
// Oracle 4: Process context (Linux procfs; empty elsewhere)
// ============================================================================

typedef struct {
    uint64_t start_addr;
    uint64_t end_addr;
    uint32_t permissions;
    char     path[256];
} MemoryMapping;

MemoryMapping* bine_get_memory_maps(uint32_t* n_maps) {
    *n_maps = 0;
#if defined(__linux__)
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return NULL;

    uint32_t count = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) count++;
    fseek(f, 0, SEEK_SET);

    MemoryMapping* maps = (MemoryMapping*)calloc(count, sizeof(MemoryMapping));
    if (!maps) { fclose(f); return NULL; }

    uint32_t idx = 0;
    while (fgets(line, sizeof(line), f) && idx < count) {
        uint64_t start, end;
        char perms[5];
        char path[256];

        if (sscanf(line, "%lx-%lx %4s %*x %*x:%*x %*u %255s",
                   &start, &end, perms, path) == 4) {
            maps[idx].start_addr = start;
            maps[idx].end_addr = end;
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
#else
    return NULL;
#endif
}

void bine_free_memory_maps(MemoryMapping* maps) {
    free(maps);
}

// Get open file descriptors
char** bine_get_open_fds(uint32_t* n_fds) {
    *n_fds = 0;
#if defined(__linux__)
    char path[256];
    uint32_t count = 0;
    for (int i = 0; i < 1024; i++) {
        snprintf(path, sizeof(path), "/proc/self/fd/%d", i);
        if (access(path, F_OK) == 0) count++;
    }
    if (count == 0) return NULL;

    char** fds = (char**)calloc(count, sizeof(char*));
    if (!fds) return NULL;
    uint32_t idx = 0;
    for (int i = 0; i < 1024 && idx < count; i++) {
        snprintf(path, sizeof(path), "/proc/self/fd/%d", i);
        if (access(path, F_OK) == 0) {
            fds[idx++] = strdup(path);
        }
    }
    *n_fds = idx;
    return fds;
#else
    return NULL;
#endif
}

void bine_free_fds(char** fds, uint32_t n_fds) {
    if (!fds) return;
    for (uint32_t i = 0; i < n_fds; i++) free(fds[i]);
    free(fds);
}

// Get resource usage (for priority bias)
void bine_get_resource_usage(uint64_t* utime_usec, uint64_t* stime_usec) {
    *utime_usec = 0;
    *stime_usec = 0;
#if defined(__linux__)
    FILE* f = fopen("/proc/self/stat", "r");
    if (!f) return;
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
            if (token) *stime_usec = strtoull(token, NULL, 10);
        }
    }
    fclose(f);
#endif
}

// ============================================================================
// Oracle 5: Call trace from PLT/GOT structure
// ============================================================================

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

// Extract PLT entries from .rela.plt
PLTEntry* bine_extract_plt(const BinaryIntrinsic* bin, uint32_t* n_plt) {
    if (!bin || !bin->relocs || bin->n_relocs == 0) {
        *n_plt = 0;
        return NULL;
    }

    uint32_t plt_count = 0;
    for (uint32_t i = 0; i < bin->n_relocs; i++) {
        if (bin->relocs[i].type == R_X86_64_JUMP_SLOT) plt_count++;
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

// Build call graph from decoded .text
CallEdge* bine_build_call_graph(const BinaryIntrinsic* bin, uint32_t* n_edges) {
    *n_edges = 0;
    if (!bin) return NULL;

    uint32_t n_insns = 0;
    Instruction* insns = decode_instructions(bin->text_bytes, bin->text_size, &n_insns);
    if (!insns || n_insns == 0) return NULL;

    uint32_t call_count = 0;
    for (uint32_t i = 0; i < n_insns; i++) {
        if (insns[i].is_call) call_count++;
    }
    if (call_count == 0) {
        free_instructions(insns);
        return NULL;
    }

    CallEdge* edges = (CallEdge*)calloc(call_count, sizeof(CallEdge));
    if (!edges) {
        free_instructions(insns);
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

// Estimate call frequency from symbol size (proxy for call site count)
uint32_t bine_estimate_call_frequency(const Symbol* sym, const BinaryIntrinsic* bin) {
    (void)bin;
    return sym->size;
}

void bine_free_plt(PLTEntry* plt, uint32_t n_plt) {
    if (!plt) return;
    for (uint32_t i = 0; i < n_plt; i++) free(plt[i].symbol_name);
    free(plt);
}

void bine_free_call_graph(CallEdge* edges) {
    free(edges);
}
