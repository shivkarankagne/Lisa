#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#define LISA_STORAGE_MAX_HANDLES 64

typedef struct {
    int in_use;
    char* path;
    int n;
    int dim;
    float* vectors;
} storage_slot_t;

static storage_slot_t g_slots[LISA_STORAGE_MAX_HANDLES];

static const char LISA_MAGIC[4] = { 'L', 'I', 'S', 'A' };

/* ---- helpers ---- */

static int write_u32_le(FILE* f, uint32_t v) {
    unsigned char buf[4];
    buf[0] = (unsigned char)( v        & 0xFFu);
    buf[1] = (unsigned char)((v >>  8) & 0xFFu);
    buf[2] = (unsigned char)((v >> 16) & 0xFFu);
    buf[3] = (unsigned char)((v >> 24) & 0xFFu);
    return fwrite(buf, 1, 4, f) == 4 ? 0 : -2;
}

static int read_u32_le(FILE* f, uint32_t* out) {
    unsigned char buf[4];
    if (fread(buf, 1, 4, f) != 4) return -3;
    *out = (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
    return 0;
}

static int dir_exists(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

static char* path_join(const char* dir, const char* name) {
    size_t dl = strlen(dir);
    size_t nl = strlen(name);
    size_t need = dl + 1 + nl + 1;
    char* p = (char*)malloc(need);
    if (!p) return NULL;
    memcpy(p, dir, dl);
    p[dl] = '/';
    memcpy(p + dl + 1, name, nl);
    p[dl + 1 + nl] = '\0';
    return p;
}

static int handle_valid(int handle) {
    return handle > 0 && handle < LISA_STORAGE_MAX_HANDLES
        && g_slots[handle].in_use;
}

static int handle_alloc(void) {
    for (int i = 1; i < LISA_STORAGE_MAX_HANDLES; i++) {
        if (!g_slots[i].in_use) return i;
    }
    return -6;
}

/* ---- public API ---- */

int storage_create(const char* path, int n, int dim, const float* vectors) {
    if (!path || !vectors) return -1;
    if (n <= 0 || dim <= 0) return -1;
    if (strlen(path) == 0) return -1;

    if (dir_exists(path)) return -2;
    if (mkdir(path, 0755) != 0) return -2;

    char* header_path = path_join(path, "header.bin");
    char* vectors_path = path_join(path, "vectors.bin");
    if (!header_path || !vectors_path) {
        free(header_path); free(vectors_path);
        return -5;
    }

    FILE* hf = fopen(header_path, "wb");
    if (!hf) {
        free(header_path); free(vectors_path);
        return -2;
    }
    int rc = 0;
    if (fwrite(LISA_MAGIC, 1, 4, hf) != 4) rc = -2;
    if (rc == 0 && write_u32_le(hf, LISA_STORAGE_FORMAT_VERSION) != 0) rc = -2;
    if (rc == 0 && write_u32_le(hf, (uint32_t)n) != 0) rc = -2;
    if (rc == 0 && write_u32_le(hf, (uint32_t)dim) != 0) rc = -2;
    if (fclose(hf) != 0 && rc == 0) rc = -2;
    if (rc != 0) {
        free(header_path); free(vectors_path);
        return rc;
    }

    FILE* vf = fopen(vectors_path, "wb");
    if (!vf) {
        free(header_path); free(vectors_path);
        return -2;
    }
    size_t count = (size_t)n * (size_t)dim;
    if (fwrite(vectors, sizeof(float), count, vf) != count) {
        fclose(vf);
        free(header_path); free(vectors_path);
        return -2;
    }
    if (fclose(vf) != 0) {
        free(header_path); free(vectors_path);
        return -2;
    }

    free(header_path); free(vectors_path);
    return 0;
}

int storage_open(const char* path) {
    if (!path) return -1;
    if (!dir_exists(path)) return -2;

    char* header_path = path_join(path, "header.bin");
    char* vectors_path = path_join(path, "vectors.bin");
    if (!header_path || !vectors_path) {
        free(header_path); free(vectors_path);
        return -5;
    }

    FILE* hf = fopen(header_path, "rb");
    if (!hf) {
        free(header_path); free(vectors_path);
        return -2;
    }

    char magic[4];
    if (fread(magic, 1, 4, hf) != 4) {
        fclose(hf); free(header_path); free(vectors_path);
        return -3;
    }
    if (memcmp(magic, LISA_MAGIC, 4) != 0) {
        fclose(hf); free(header_path); free(vectors_path);
        return -3;
    }

    uint32_t version = 0, n_u = 0, dim_u = 0;
    if (read_u32_le(hf, &version) != 0 ||
        read_u32_le(hf, &n_u) != 0 ||
        read_u32_le(hf, &dim_u) != 0) {
        fclose(hf); free(header_path); free(vectors_path);
        return -3;
    }
    fclose(hf);

    if (version != LISA_STORAGE_FORMAT_VERSION) {
        free(header_path); free(vectors_path);
        return -3;
    }

    int n = (int)n_u;
    int dim = (int)dim_u;
    if (n <= 0 || dim <= 0) {
        free(header_path); free(vectors_path);
        return -3;
    }

    size_t count = (size_t)n * (size_t)dim;
    float* vectors = (float*)malloc(count * sizeof(float));
    if (!vectors) {
        free(header_path); free(vectors_path);
        return -5;
    }

    FILE* vf = fopen(vectors_path, "rb");
    if (!vf) {
        free(vectors);
        free(header_path); free(vectors_path);
        return -2;
    }
    if (fread(vectors, sizeof(float), count, vf) != count) {
        fclose(vf);
        free(vectors);
        free(header_path); free(vectors_path);
        return -3;
    }
    fclose(vf);

    int handle = handle_alloc();
    if (handle < 0) {
        free(vectors);
        free(header_path); free(vectors_path);
        return -6;
    }

    g_slots[handle].in_use = 1;
    g_slots[handle].path = (char*)malloc(strlen(path) + 1);
    if (!g_slots[handle].path) {
        g_slots[handle].in_use = 0;
        free(vectors);
        free(header_path); free(vectors_path);
        return -5;
    }
    strcpy(g_slots[handle].path, path);
    g_slots[handle].n = n;
    g_slots[handle].dim = dim;
    g_slots[handle].vectors = vectors;

    free(header_path); free(vectors_path);
    return handle;
}

int storage_get_dim(int handle) {
    if (!handle_valid(handle)) return -1;
    return g_slots[handle].dim;
}

int storage_get_n(int handle) {
    if (!handle_valid(handle)) return -1;
    return g_slots[handle].n;
}

const float* storage_get_vectors(int handle) {
    if (!handle_valid(handle)) return NULL;
    return g_slots[handle].vectors;
}

int storage_close(int handle) {
    if (!handle_valid(handle)) return -1;
    free(g_slots[handle].path);
    free(g_slots[handle].vectors);
    g_slots[handle].path = NULL;
    g_slots[handle].vectors = NULL;
    g_slots[handle].n = 0;
    g_slots[handle].dim = 0;
    g_slots[handle].in_use = 0;
    return 0;
}
