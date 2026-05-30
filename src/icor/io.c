// icor/io.c — load/save raw bit strings

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <cuda_runtime.h>

// LOAD: read the raw bit string from disk, skip 12-byte header,
// upload the raw agent words directly to GPU in one call.
uint64_t* icor_load(const char* path, uint32_t* out_n, uint32_t* out_root) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open file: %s\n", path);
        return NULL;
    }

    uint8_t header[12];
    fread(header, 1, 12, f);

    // Verify magic bytes
    if (header[0] != 0x49 || header[1] != 0x43 || header[2] != 0x4F || header[3] != 0x52) {
        fprintf(stderr, "Invalid ICOR magic bytes\n");
        fclose(f);
        return NULL;
    }

    // Extract agent count (24-bit big-endian from bytes 4-6)
    uint32_t n = ((uint32_t)header[4] << 16) |
                 ((uint32_t)header[5] << 8) |
                 (uint32_t)header[6];

    // Extract root portref from bytes 8-11 (20 bits in high bits, 12-bit padding)
    uint32_t rp = ((uint32_t)header[8] << 24) |
                  ((uint32_t)header[9] << 16) |
                  ((uint32_t)header[10] << 8) |
                  (uint32_t)header[11];
    uint32_t root = rp >> 12;  // Extract 20-bit portref from high bits

    *out_n = n;
    *out_root = root;

    uint64_t* d_net;
    cudaMalloc((void**)&d_net, sizeof(uint64_t) * (n + 4096));  // +4096 slack for alloc

    // Single H2D transfer: the raw agent words start at byte 12 of the file
    uint64_t* h_buf = (uint64_t*)malloc(8 * n);
    fread(h_buf, 8, n, f);
    cudaMemcpy(d_net, h_buf, 8 * n, cudaMemcpyHostToDevice);
    free(h_buf);
    fclose(f);

    return d_net;
}

// SAVE: download raw agent words from GPU, prepend 12-byte header, write to disk.
void icor_save(const char* path, uint64_t* d_net, uint32_t n, uint32_t root) {
    uint64_t* h_buf = (uint64_t*)malloc(8 * n);
    cudaMemcpy(h_buf, d_net, 8 * n, cudaMemcpyDeviceToHost);  // single D2H

    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Failed to open file for writing: %s\n", path);
        free(h_buf);
        return;
    }

    // Write 12-byte header
    uint8_t header[12];
    // Bytes 0-3: magic
    header[0] = 'I';
    header[1] = 'C';
    header[2] = 'O';
    header[3] = 'R';
    // Bytes 4-6: agent count, 24-bit big-endian (disjoint from root portref)
    header[4] = (n >> 16) & 0xFF;
    header[5] = (n >> 8) & 0xFF;
    header[6] = n & 0xFF;
    // Byte 7: flags (reserved, write 0)
    header[7] = 0x00;
    // Bytes 8-11: root portref (20 bits in high bits, 12-bit padding in low bits)
    uint32_t rp = (root & 0xFFFFFu) << 12;  // 20 bits → top of uint32
    header[8] = (rp >> 24) & 0xFF;
    header[9] = (rp >> 16) & 0xFF;
    header[10] = (rp >> 8) & 0xFF;
    header[11] = rp & 0xFF;

    fwrite(header, 1, 12, f);
    fwrite(h_buf, 8, n, f);

    free(h_buf);
    fclose(f);
}
