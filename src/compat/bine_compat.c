/*
 * bine_compat.c - Portable platform shim implementations.
 * See include/bine_compat.h for the interface contract.
 */

#include "bine_compat.h"

#if defined(BINE_WINDOWS)
#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <direct.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#endif

/* ============================================================================
 * Time
 * ============================================================================
 */

#if defined(BINE_WINDOWS)
#include <windows.h>

double bine_now_sec(void) {
    static int have_freq = 0;
    static double inv_freq = 1.0;
    if (!have_freq) {
        LARGE_INTEGER q;
        if (QueryPerformanceFrequency(&q) && q.QuadPart > 0) {
            inv_freq = 1.0 / (double)q.QuadPart;
            have_freq = 1;
        }
    }
    if (have_freq) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        return (double)q.QuadPart * inv_freq;
    }
    return (double)GetTickCount64() / 1000.0;
}

uint64_t bine_time_ms(void) {
    double s = bine_now_sec();
    return (uint64_t)(s * 1000.0);
}
#else
double bine_now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

uint64_t bine_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}
#endif

/* ============================================================================
 * File I/O
 * ============================================================================
 */

#if defined(BINE_WINDOWS)
#define BINE_O_RDONLY _O_RDONLY
#define BINE_O_WRONLY (_O_WRONLY | _O_CREAT | _O_TRUNC)
#define BINE_MODE_0755 (_S_IREAD | _S_IWRITE)
#else
#define BINE_O_RDONLY O_RDONLY
#define BINE_O_WRONLY (O_WRONLY | O_CREAT | O_TRUNC)
#define BINE_MODE_0755 0755
#endif

int bine_fopen_rd(const char *path) {
#if defined(BINE_WINDOWS)
    return _open(path, BINE_O_RDONLY | _O_BINARY);
#else
    return open(path, BINE_O_RDONLY);
#endif
}

int bine_fopen_wr(const char *path) {
#if defined(BINE_WINDOWS)
    return _open(path, BINE_O_WRONLY | _O_BINARY, BINE_MODE_0755);
#else
    return open(path, BINE_O_WRONLY, BINE_MODE_0755);
#endif
}

int64_t bine_fsize(int fd) {
    if (fd < 0) return -1;
#if defined(BINE_WINDOWS)
    {
        struct _stat64 st;
        if (_fstat64(fd, &st) != 0) return -1;
        return (int64_t)st.st_size;
    }
#else
    {
        struct stat st;
        if (fstat(fd, &st) != 0) return -1;
        return (int64_t)st.st_size;
    }
#endif
}

size_t bine_fread_fd(int fd, void *buf, size_t n) {
    if (fd < 0 || !buf) return (size_t)-1;
#if defined(BINE_WINDOWS)
    return (size_t)_read(fd, buf, (unsigned)n);
#else
    return (size_t)read(fd, buf, n);
#endif
}

size_t bine_fwrite_fd(int fd, const void *buf, size_t n) {
    if (fd < 0 || !buf) return (size_t)-1;
#if defined(BINE_WINDOWS)
    return (size_t)_write(fd, buf, (unsigned)n);
#else
    return (size_t)write(fd, buf, n);
#endif
}

void bine_fclose(int fd) {
    if (fd >= 0) {
#if defined(BINE_WINDOWS)
        _close(fd);
#else
        close(fd);
#endif
    }
}

int bine_file_copy(const char *src, const char *dst) {
    uint8_t *buf = NULL;
    size_t len = 0;
    int rc = -1;
    if (bine_read_file(src, &buf, &len) < 0) return -1;
    if (bine_write_file(dst, buf, len) == 0) rc = 0;
    free(buf);
    if (rc == 0) bine_chmod_exec(dst);
    return rc;
}

int bine_read_file(const char *path, uint8_t **out, size_t *out_len) {
    if (!path || !out || !out_len) return -1;
    int fd = bine_fopen_rd(path);
    if (fd < 0) return -1;
    int64_t sz = bine_fsize(fd);
    if (sz < 0) { bine_fclose(fd); return -1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz + 1);
    if (!buf) { bine_fclose(fd); return -1; }
    size_t got = bine_fread_fd(fd, buf, (size_t)sz);
    bine_fclose(fd);
    if (got != (size_t)sz) { free(buf); return -1; }
    buf[sz] = 0; /* NUL terminator for text-oriented consumers */
    *out = buf;
    *out_len = (size_t)sz;
    return 0;
}

int bine_write_file(const char *path, const uint8_t *buf, size_t len) {
    if (!path || !buf) return -1;
    int fd = bine_fopen_wr(path);
    if (fd < 0) return -1;
    size_t w = bine_fwrite_fd(fd, buf, len);
    bine_fclose(fd);
    return (w == len) ? 0 : -1;
}

int bine_chmod_exec(const char *path) {
    if (!path) return -1;
#if defined(BINE_WINDOWS)
    /* Windows derives executable-ness from the extension/loader, not bits. */
    (void)path;
    return 0;
#else
    if (chmod(path, 0755) != 0) return -1;
    return 0;
#endif
}

const char *bine_state_dir(void) {
#if defined(BINE_WINDOWS)
    static char dir[1024];
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
    if (!tmp) tmp = ".";
    snprintf(dir, sizeof(dir), "%s\\bine", tmp);
    {
        struct _stat64 st;
        if (_stat64(dir, &st) != 0) {
            _mkdir(dir);
        }
    }
    return dir;
#else
    {
        struct stat st;
        if (stat("/tmp/bine", &st) != 0) {
            mkdir("/tmp/bine", 0700);
        }
    }
    return "/tmp/bine";
#endif
}

/* ============================================================================
 * Subprocess capture
 * ============================================================================
 */

int bine_run_capture(const char *cmd, char **out, size_t *out_len, int *exit_status) {
    if (!cmd || !out || !out_len || !exit_status) return -1;
    *out = NULL;
    *out_len = 0;
    *exit_status = -1;

#if defined(BINE_WINDOWS)
    FILE *fp = _popen(cmd, "r");
#else
    FILE *fp = popen(cmd, "r");
#endif
    if (!fp) return -1;

    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
#if defined(BINE_WINDOWS)
        _pclose(fp);
#else
        pclose(fp);
#endif
        return -1;
    }
    size_t r;
    while ((r = fread(buf + len, 1, cap - len, fp)) > 0) {
        len += r;
        if (len == cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
        }
    }
#if defined(BINE_WINDOWS)
    int st = _pclose(fp);
#else
    int st = pclose(fp);
#endif
    if (!buf) return -1;
    if (len == cap) {
        char *nb = (char *)realloc(buf, cap + 1);
        if (!nb) { free(buf); return -1; }
        buf = nb;
    }
    buf[len] = 0;
    *out = buf;
    *out_len = len;

    /* Normalize exit status: WEXITSTATUS on POSIX; _pclose already gives it. */
#if !defined(BINE_WINDOWS)
    if (WIFEXITED(st)) {
        *exit_status = WEXITSTATUS(st);
    } else {
        *exit_status = 128 + (int)(st & 0x7f); /* killed by signal */
    }
#else
    *exit_status = st;
#endif
    return 0;
}

int bine_strcasecmp(const char *a, const char *b) {
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
#if defined(_MSC_VER) && !defined(__clang__)
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}
