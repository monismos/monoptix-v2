/*
 * bine_compat.h - Portable platform shims for BINE.
 *
 * BINE targets Linux/ELF natively, but is engineered to build and run on
 * Windows (MSVC and MinGW) as well, both for CPU-only and CUDA (Blackwell)
 * configurations.  This header centralizes every platform difference:
 *
 *   - 128-bit integers (MSVC has no __uint128_t)
 *   - <elf.h> (Linux-only; definitions provided here for other platforms)
 *   - file I/O, process capture, time, chmod
 *   - cache directory
 *
 * The Linux/POSIX code paths are preserved verbatim; non-POSIX platforms
 * get behavioral equivalents so the optimizer logic is identical everywhere.
 */

#ifndef BINE_COMPAT_H
#define BINE_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define BINE_WINDOWS 1
#else
#  define BINE_POSIX 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 128-bit integer type
 * ============================================================================
 *
 * GCC/Clang/nvcc provide __uint128_t on both Linux and Windows MinGW.
 * MSVC does not; there we fall back to a lo/hi pair with explicit helpers.
 * Keep operations explicit so both backends behave identically.
 */

#if defined(_MSC_VER) && !defined(__clang__)
typedef struct { uint64_t lo, hi; } bine_u128;
#else
typedef __uint128_t bine_u128;
#endif

static inline bine_u128 bine_u128_from64(uint64_t v) {
#if defined(_MSC_VER) && !defined(__clang__)
    bine_u128 r; r.lo = v; r.hi = 0; return r;
#else
    return (bine_u128)v;
#endif
}

static inline uint64_t bine_u128_lo(bine_u128 a) {
#if defined(_MSC_VER) && !defined(__clang__)
    return a.lo;
#else
    return (uint64_t)a;
#endif
}

static inline uint64_t bine_u128_hi(bine_u128 a) {
#if defined(_MSC_VER) && !defined(__clang__)
    return a.hi;
#else
    return (uint64_t)(a >> 64);
#endif
}

static inline bine_u128 bine_u128_or(bine_u128 a, bine_u128 b) {
#if defined(_MSC_VER) && !defined(__clang__)
    bine_u128 r; r.lo = a.lo | b.lo; r.hi = a.hi | b.hi; return r;
#else
    return a | b;
#endif
}

static inline bine_u128 bine_u128_and(bine_u128 a, bine_u128 b) {
#if defined(_MSC_VER) && !defined(__clang__)
    bine_u128 r; r.lo = a.lo & b.lo; r.hi = a.hi & b.hi; return r;
#else
    return a & b;
#endif
}

static inline bine_u128 bine_u128_xor(bine_u128 a, bine_u128 b) {
#if defined(_MSC_VER) && !defined(__clang__)
    bine_u128 r; r.lo = a.lo ^ b.lo; r.hi = a.hi ^ b.hi; return r;
#else
    return a ^ b;
#endif
}

static inline bine_u128 bine_u128_not(bine_u128 a) {
#if defined(_MSC_VER) && !defined(__clang__)
    bine_u128 r; r.lo = ~a.lo; r.hi = ~a.hi; return r;
#else
    return ~a;
#endif
}

static inline int bine_u128_iszero(bine_u128 a) {
#if defined(_MSC_VER) && !defined(__clang__)
    return a.lo == 0 && a.hi == 0;
#else
    return a == 0;
#endif
}

static inline bine_u128 bine_u128_shl(bine_u128 a, unsigned n) {
#if defined(_MSC_VER) && !defined(__clang__)
    bine_u128 r;
    if (n == 0) { r = a; }
    else if (n >= 64) { r.lo = 0; r.hi = a.lo << (n - 64); }
    else { r.lo = a.lo << n; r.hi = (a.hi << n) | (a.lo >> (64 - n)); }
    return r;
#else
    return a << n;
#endif
}

static inline int bine_u128_popcount(bine_u128 a) {
#if defined(_MSC_VER) && !defined(__clang__)
    int c = 0;
    uint64_t x;
    x = a.lo; while (x) { c += (int)(x & 1); x >>= 1; }
    x = a.hi; while (x) { c += (int)(x & 1); x >>= 1; }
    return c;
#else
    /* __builtin_popcountll is available in MSVC mode via clang-cl too,
       but the pair path above is fine; use it everywhere for determinism. */
    int c = 0;
    uint64_t x = (uint64_t)a;
    while (x) { c += (int)(x & 1); x >>= 1; }
    x = (uint64_t)(a >> 64);
    while (x) { c += (int)(x & 1); x >>= 1; }
    return c;
#endif
}

/* ============================================================================
 * <elf.h> compatibility
 * ============================================================================
 * Linux provides <elf.h>.  Windows/MSVC and MinGW do not.  When it is
 * unavailable we define exactly the subset of ELF64 that BINE consumes.
 * All definitions match glibc's <elf.h> values so behavior is identical.
 */

#if !defined(BINE_HAVE_ELF_H)
#  if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__) || defined(__APPLE__)
#    include <elf.h>
#    define BINE_HAVE_ELF_H 1
#  else
#    define BINE_HAVE_ELF_H 0
#  endif
#endif

#if !BINE_HAVE_ELF_H

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

typedef struct {
    unsigned char e_ident[16];
    Elf64_Half    e_type;
    Elf64_Half    e_machine;
    Elf64_Word    e_version;
    Elf64_Addr    e_entry;
    Elf64_Off     e_phoff;
    Elf64_Off     e_shoff;
    Elf64_Word    e_flags;
    Elf64_Half    e_ehsize;
    Elf64_Half    e_phentsize;
    Elf64_Half    e_phnum;
    Elf64_Half    e_shentsize;
    Elf64_Half    e_shnum;
    Elf64_Half    e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word    sh_name;
    Elf64_Word    sh_type;
    Elf64_Xword   sh_flags;
    Elf64_Addr    sh_addr;
    Elf64_Off     sh_offset;
    Elf64_Xword   sh_size;
    Elf64_Word    sh_link;
    Elf64_Word    sh_info;
    Elf64_Xword   sh_addralign;
    Elf64_Xword   sh_entsize;
} Elf64_Shdr;

typedef struct {
    Elf64_Word    p_type;
    Elf64_Word    p_flags;
    Elf64_Off     p_offset;
    Elf64_Addr    p_vaddr;
    Elf64_Addr    p_paddr;
    Elf64_Xword   p_filesz;
    Elf64_Xword   p_memsz;
    Elf64_Xword   p_align;
} Elf64_Phdr;

typedef struct {
    Elf64_Word    st_name;
    unsigned char st_info;
    unsigned char st_other;
    Elf64_Half    st_shndx;
    Elf64_Addr    st_value;
    Elf64_Xword   st_size;
} Elf64_Sym;

typedef struct {
    Elf64_Addr    r_offset;
    Elf64_Xword   r_info;
    Elf64_Sxword  r_addend;
} Elf64_Rela;

typedef struct {
    Elf64_Sxword  d_tag;
    union { Elf64_Xword d_val; Elf64_Addr d_ptr; } d_un;
} Elf64_Dyn;

#define ELFMAG          "\177ELF"
#define SELFMAG         4

/* EI_* indices into e_ident */
#define EI_MAG0         0
#define EI_MAG1         1
#define EI_MAG2         2
#define EI_MAG3         3
#define EI_CLASS        4
#define EI_DATA         5
#define EI_VERSION      6
#define EI_OSABI        7

#define ELFCLASS32      1
#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define ELFDATA2MSB     2
#define EV_CURRENT      1
#define ELFOSABI_SYSV   0
#define ELFOSABI_LINUX  3

#define ET_EXEC         2
#define ET_DYN          3

#define EM_386          3
#define EM_X86_64       62
#define EM_AARCH64      183
#define EM_RISCV        243

#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_STACK    0x6474e551
#define PT_GNU_RELRO    0x6474e552

#define PF_X            1
#define PF_W            2
#define PF_R            4

#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_RELA        4
#define SHT_HASH        5
#define SHT_DYNAMIC     6
#define SHT_NOTE        7
#define SHT_NOBITS      8
#define SHT_REL         9
#define SHT_DYNSYM      11
#define SHT_INIT_ARRAY  14
#define SHT_FINI_ARRAY  15
#define SHT_PREINIT_ARRAY 16
#define SHT_GNU_HASH    0x6ffffff6
#define SHT_GNU_versym  0x6fffffff
#define SHT_GNU_verneed 0x6ffffffe

#define SHF_WRITE       0x1
#define SHF_ALLOC       0x2
#define SHF_EXECINSTR   0x4
#define SHF_STRINGS     0x20
#define SHF_INFO_LINK   0x40

#define SHN_UNDEF       0
#define SHN_ABS         0xfff1
#define SHN_COMMON      0xfff2

#define STN_UNDEF       0

#define STT_NOTYPE      0
#define STT_OBJECT      1
#define STT_FUNC        2

#define STB_LOCAL       0
#define STB_GLOBAL      1
#define STB_WEAK        2

#define R_X86_64_NONE   0
#define R_X86_64_64     1
#define R_X86_64_PC32   2
#define R_X86_64_GOT32  3
#define R_X86_64_PLT32  4
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_32     10
#define R_X86_64_32S    11
#define R_X86_64_IRELATIVE 37

#define DT_NULL         0
#define DT_NEEDED       1
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_STRSZ        10
#define DT_SYMENT       11
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_RELAENT      9
#define DT_PLTREL       20
#define DT_PLTRELSZ     2
#define DT_JMPREL       23
#define DT_BIND_NOW     24
#define DT_GNU_HASH     0x6ffffef5
#define DT_HASH         4

#define NT_GNU_BUILD_ID 3

#define ELF64_R_SYM(i)   ((i) >> 32)
#define ELF64_R_TYPE(i)  ((i) & 0xffffffffL)
#define ELF64_R_INFO(s, t) (((s) << 32) + ((t) & 0xffffffffL))

#define ELF64_ST_BIND(i)  ((i) >> 4)
#define ELF64_ST_TYPE(i)  ((i) & 0xf)
#define ELF64_ST_INFO(b, t) (((b) << 4) + ((t) & 0xf))

#define ELF64_ST_VISIBILITY(o) ((o) & 3)

#endif /* !BINE_HAVE_ELF_H */

/* ============================================================================
 * Portable time
 * ============================================================================
 */

double bine_now_sec(void);   /* monotonic seconds */
uint64_t bine_time_ms(void); /* monotonic milliseconds */

/* ============================================================================
 * Portable file I/O
 * ============================================================================
 */

/* Open a file for reading; returns fd or -1. */
int bine_fopen_rd(const char *path);
/* Open/create a file for writing (truncating); returns fd or -1. */
int bine_fopen_wr(const char *path);
/* File size in bytes for an open fd; -1 on error. */
int64_t bine_fsize(int fd);
/* Read exactly up to n bytes; returns bytes read or -1. */
size_t bine_fread_fd(int fd, void *buf, size_t n);
/* Write n bytes; returns n or -1. */
size_t bine_fwrite_fd(int fd, const void *buf, size_t n);
/* Close an fd. */
void bine_fclose(int fd);
/* Byte-exact copy src -> dst (creates/truncates dst). Returns 0 or -1. */
int bine_file_copy(const char *src, const char *dst);
/* Read whole file into malloc'd buffer; returns 0 or -1. */
int bine_read_file(const char *path, uint8_t **out, size_t *out_len);
/* Write whole buffer to file; returns 0 or -1. */
int bine_write_file(const char *path, const uint8_t *buf, size_t len);
/* Make a file executable (no-op on Windows). */
int bine_chmod_exec(const char *path);
/* Directory for transient state (cache). No trailing slash. */
const char *bine_state_dir(void);

/* ============================================================================
 * Portable subprocess capture
 * ============================================================================
 *
 * Runs `cmd` through the platform shell, captures combined output and the
 * process exit status (normalized: -1 on spawn failure).  Returns 0 on
 * success, -1 on failure.  The caller must free *out.
 */
int bine_run_capture(const char *cmd, char **out, size_t *out_len, int *exit_status);

/* String case-insensitive compare (strcasecmp shim for MSVC). */
int bine_strcasecmp(const char *a, const char *b);

/* ssize_t / strdup / snprintf shims for MSVC without C99 extensions. */
#if defined(_MSC_VER) && !defined(__clang__)
#ifndef ssize_t
typedef intptr_t ssize_t;
#endif
#ifndef strdup
#define strdup _strdup
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif /* BINE_COMPAT_H */
