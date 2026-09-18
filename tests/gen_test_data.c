/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gen_test_data — deterministic test data for the CLI and API tests.
 *
 * Usage:
 *   gen_test_data <index_out> <query_out> <n> <dim> <seed>
 *
 * Writes:
 *   index_out  binary index file: int32 n, int32 dim (LE), then n * dim
 *              float32 LE values, row-major (see src/cli/README.md)
 *   query_out  text file: dim whitespace-separated floats
 *
 * Values are uniform in [0, 1) from a fixed xorshift generator, so the
 * output is identical on every platform and compiler.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static uint32_t g_state;

static float next_float(void) {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 17;
    g_state ^= g_state << 5;
    return (float)(g_state >> 8) / 16777216.0f;
}

static int write_u32_le(FILE* f, uint32_t v) {
    unsigned char b[4] = {
        (unsigned char)(v & 0xff), (unsigned char)((v >> 8) & 0xff),
        (unsigned char)((v >> 16) & 0xff), (unsigned char)((v >> 24) & 0xff)
    };
    return fwrite(b, 1, 4, f) == 4 ? 0 : -1;
}

int main(int argc, char** argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s <index_out> <query_out> <n> <dim> <seed>\n", argv[0]);
        return 1;
    }
    long n = strtol(argv[3], NULL, 10);
    long dim = strtol(argv[4], NULL, 10);
    long seed = strtol(argv[5], NULL, 10);
    if (n <= 0 || dim <= 0 || n > 10000000 || dim > 65536) {
        fprintf(stderr, "error: invalid n or dim\n");
        return 1;
    }
    g_state = (uint32_t)seed ? (uint32_t)seed : 1u;

    FILE* fi = fopen(argv[1], "wb");
    if (!fi) {
        fprintf(stderr, "error: cannot open %s\n", argv[1]);
        return 2;
    }
    if (write_u32_le(fi, (uint32_t)n) || write_u32_le(fi, (uint32_t)dim)) {
        fclose(fi);
        return 2;
    }
    for (long i = 0; i < n * dim; i++) {
        float v = next_float();
        uint32_t bits;
        memcpy(&bits, &v, sizeof(bits));
        if (write_u32_le(fi, bits)) {
            fclose(fi);
            return 2;
        }
    }
    fclose(fi);

    FILE* fq = fopen(argv[2], "w");
    if (!fq) {
        fprintf(stderr, "error: cannot open %s\n", argv[2]);
        return 2;
    }
    for (long i = 0; i < dim; i++) {
        fprintf(fq, "%s%.9g", i ? " " : "", (double)next_float());
    }
    fprintf(fq, "\n");
    fclose(fq);
    return 0;
}
