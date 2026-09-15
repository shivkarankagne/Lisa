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
#define LISA_STORAGE_HEADER_V1 16
#define LISA_STORAGE_HEADER_V2 20

typedef struct {
    int in_use;
    char* path;
    int n;
    int dim;
    int capacity;
    int version;
    float* vectors;
} storage_slot_t;

static storage_slot_t g_slots[LISA_STORAGE_MAX_HANDLES];

static const char LISA_MAGIC[4] = { 'L', 'I', 'S', 'A' };

/* ---- little-endian helpers ---- */

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

/* ---- filesystem helpers ---- */

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

/* ---- handle helpers ---- */

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

/* ---- header I/O ---- */

static int write_header_v2(const char* path, int n, int dim, int capacity) {
    char* hp = path_join(path, "header.bin");
    if (!hp) return -5;
    FILE* f = fopen(hp, "wb");
    free(hp);
    if (!f) return -2;

    int rc = 0;
    if (fwrite(LISA_MAGIC, 1, 4, f) != 4) rc = -2;
    if (rc == 0 && write_u32_le(f, LISA_STORAGE_FORMAT_VERSION) != 0) rc = -2;
    if (rc == 0 && write_u32_le(f, (uint32_t)n) != 0) rc = -2;
    if (rc == 0 && write_u32_le(f, (uint32_t)dim) != 0) rc = -2;
    if (rc == 0 && write_u32_le(f, (uint32_t)capacity) != 0) rc = -2;
    if (fclose(f) != 0 && rc == 0) rc = -2;
    return rc;
}

static int write_vectors(const char* path, const float* vectors,
                         int n_live, int dim, int capacity) {
    char* vp = path_join(path, "vectors.bin");
    if (!vp) return -5;
    FILE* f = fopen(vp, "wb");
    free(vp);
    if (!f) return -2;

    size_t total = (size_t)capacity * (size_t)dim;
    size_t live  = (size_t)n_live   * (size_t)dim;

    int rc = 0;
    if (live > 0) {
        if (fwrite(vectors, sizeof(float), live, f) != live) rc = -2;
    }
    if (rc == 0 && total > live) {
        /* zero the unused tail so the file is deterministic */
        size_t tail = total - live;
        float* zeros = (float*)calloc(tail, sizeof(float));
        if (!zeros) {
            rc = -5;
        } else {
            if (fwrite(zeros, sizeof(float), tail, f) != tail) rc = -2;
            free(zeros);
        }
    }
    if (fclose(f) != 0 && rc == 0) rc = -2;
    return rc;
}

/* ---- public API ---- */

int storage_create(const char* path, int n, int dim, const float* vectors) {
    if (!path || !vectors) return -1;
    if (n <= 0 || dim <= 0) return -1;
    if (strlen(path) == 0) return -1;
    if (dir_exists(path)) return -2;

    if (mkdir(path, 0755) != 0) return -2;

    int capacity = n;
    int rc = write_header_v2(path, n, dim, capacity);
    if (rc != 0) return rc;

    rc = write_vectors(path, vectors, n, dim, capacity);
    if (rc != 0) return rc;

    return 0;
}

int storage_open(const char* path) {
    if (!path) return -1;
    if (!dir_exists(path)) return -2;

    char* hp = path_join(path, "header.bin");
    char* vp = path_join(path, "vectors.bin");
    if (!hp || !vp) {
        free(hp); free(vp);
        return -5;
    }

    FILE* hf = fopen(hp, "rb");
    if (!hf) { free(hp); free(vp); return -2; }

    char magic[4];
    if (fread(magic, 1, 4, hf) != 4) {
        fclose(hf); free(hp); free(vp);
        return -3;
    }
    if (memcmp(magic, LISA_MAGIC, 4) != 0) {
        fclose(hf); free(hp); free(vp);
        return -3;
    }

    uint32_t version = 0, n_u = 0, dim_u = 0, cap_u = 0;
    if (read_u32_le(hf, &version) != 0 ||
        read_u32_le(hf, &n_u) != 0 ||
        read_u32_le(hf, &dim_u) != 0) {
        fclose(hf); free(hp); free(vp);
        return -3;
    }

    if (version == 1u) {
        cap_u = n_u;
    } else if (version == 2u) {
        if (read_u32_le(hf, &cap_u) != 0) {
            fclose(hf); free(hp); free(vp);
            return -3;
        }
    } else {
        fclose(hf); free(hp); free(vp);
        return -3;
    }
    fclose(hf);

    int n = (int)n_u;
    int dim = (int)dim_u;
    int capacity = (int)cap_u;

    if (n < 0 || dim <= 0 || capacity < n) {
        free(hp); free(vp);
        return -3;
    }

    size_t count = (size_t)capacity * (size_t)dim;
    float* vectors = NULL;
    if (count > 0) {
        vectors = (float*)malloc(count * sizeof(float));
        if (!vectors) { free(hp); free(vp); return -5; }
    }

    FILE* vf = fopen(vp, "rb");
    if (!vf) { free(vectors); free(hp); free(vp); return -2; }

    /* Live data must be present. Tail may be short for v1 files. */
    size_t live = (size_t)n * (size_t)dim;
    if (live > 0) {
        if (fread(vectors, sizeof(float), live, vf) != live) {
            fclose(vf); free(vectors); free(hp); free(vp);
            return -3;
        }
    }
    /* Zero the tail so behaviour is deterministic. */
    if (count > live) {
        memset(vectors + live, 0, (count - live) * sizeof(float));
    }
    fclose(vf);

    int handle = handle_alloc();
    if (handle < 0) {
        free(vectors); free(hp); free(vp);
        return -6;
    }

    char* path_copy = (char*)malloc(strlen(path) + 1);
    if (!path_copy) {
        free(vectors); free(hp); free(vp);
        return -5;
    }
    strcpy(path_copy, path);

    g_slots[handle].in_use = 1;
    g_slots[handle].path = path_copy;
    g_slots[handle].n = n;
    g_slots[handle].dim = dim;
    g_slots[handle].capacity = capacity;
    g_slots[handle].version = (int)version;
    g_slots[handle].vectors = vectors;

    free(hp); free(vp);
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
    g_slots[handle].capacity = 0;
    g_slots[handle].version = 0;
    g_slots[handle].in_use = 0;
    return 0;
}

int storage_insert(int handle, const float* vector) {
    if (!handle_valid(handle)) return -1;
    if (!vector) return -1;

    storage_slot_t* s = &g_slots[handle];
    if (s->version != 2) return -3;

    int dim = s->dim;
    int n = s->n;
    int capacity = s->capacity;

    if (n < capacity) {
        /* room in place: write to memory AND to disk */
        memcpy(s->vectors + (size_t)n * dim, vector, (size_t)dim * sizeof(float));
        s->n = n + 1;

        int rc = write_header_v2(s->path, s->n, dim, capacity);
        if (rc != 0) {
            s->n = n;
            return rc;
        }
        rc = write_vectors(s->path, s->vectors, s->n, dim, capacity);
        if (rc != 0) {
            s->n = n;
            return rc;
        }
        return n;
    }

    /* grow */
    int new_capacity = capacity * 2;
    if (new_capacity <= capacity) new_capacity = capacity + 1;
    if (new_capacity < 1) new_capacity = 1;

    size_t new_count = (size_t)new_capacity * (size_t)dim;
    float* new_vectors = (float*)realloc(s->vectors, new_count * sizeof(float));
    if (!new_vectors) return -5;

    memset(new_vectors + (size_t)capacity * dim, 0,
           (size_t)(new_capacity - capacity) * (size_t)dim * sizeof(float));

    memcpy(new_vectors + (size_t)n * dim, vector, (size_t)dim * sizeof(float));

    int rc = write_header_v2(s->path, n + 1, dim, new_capacity);
    if (rc != 0) return rc;

    rc = write_vectors(s->path, new_vectors, n + 1, dim, new_capacity);
    if (rc != 0) return rc;

    s->vectors = new_vectors;
    s->n = n + 1;
    s->capacity = new_capacity;
    return n;
}


int storage_delete(int handle, int index) {
    if (!handle_valid(handle)) return -1;
    if (index < 0) return -7;

    storage_slot_t* s = &g_slots[handle];
    if (s->version != 2) return -3;

    int n = s->n;
    int dim = s->dim;

    if (index >= n) return -7;

    /* compact: shift everything after index down by one */
    if (index < n - 1) {
        memmove(s->vectors + (size_t)index * dim,
                s->vectors + (size_t)(index + 1) * dim,
                (size_t)(n - index - 1) * (size_t)dim * sizeof(float));
    }
    /* zero the last live slot */
    memset(s->vectors + (size_t)(n - 1) * dim, 0, (size_t)dim * sizeof(float));

    int rc = write_header_v2(s->path, n - 1, dim, s->capacity);
    if (rc != 0) return rc;

    rc = write_vectors(s->path, s->vectors, n - 1, dim, s->capacity);
    if (rc != 0) return rc;

    s->n = n - 1;
    return 0;
}
