/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * test_store_crash — storage v2 crash safety (W2 acceptance).
 *
 * A child process runs a fixed sequence of operations (insert batches,
 * deletes, compactions) on a fresh collection and reports each committed
 * operation through a pipe. The parent kills it with SIGKILL at a random
 * moment, then reopens the collection as the writer and checks:
 *
 *   - it opens (the dead writer's lock is released, stale files cleaned);
 *   - the set of live IDs equals the simulated state after operation N or
 *     N + 1, where N is the last operation the child acknowledged (the
 *     kill may land after a commit but before its acknowledgement);
 *   - every live chunk's vector and metadata are intact.
 *
 * POSIX-only test (fork, pipe, kill).
 *
 * Usage: test_store_crash <scratch_dir> [trials]
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/platform/platform.h"
#include "../src/storage/store.h"

#define DIM      8
#define BATCH    37
#define MAX_OPS  400
#define MAX_IDS  (MAX_OPS * BATCH)
#define MODEL    "crash-test-model"

typedef struct {
    uint8_t  live[MAX_IDS];
    uint64_t next_id;
} sim_t;

static void vec_for(uint64_t id, float* out) {
    for (int j = 0; j < DIM; j++) out[j] = (float)(id * 13 + (uint64_t)j) * 0.5f;
}

enum { OP_INSERT, OP_DELETE, OP_COMPACT };

static int op_kind(int i) {
    if (i % 5 == 4) return OP_DELETE;
    if (i % 7 == 6) return OP_COMPACT;
    return OP_INSERT;
}

/* IDs deleted by a delete op: the smallest and largest live IDs. */
static int delete_targets(const sim_t* s, uint64_t out[2]) {
    int64_t lo = -1, hi = -1;
    for (uint64_t id = 0; id < s->next_id; id++) {
        if (s->live[id]) {
            if (lo < 0) lo = (int64_t)id;
            hi = (int64_t)id;
        }
    }
    if (lo < 0 || lo == hi) return 0;
    out[0] = (uint64_t)lo;
    out[1] = (uint64_t)hi;
    return 2;
}

static void sim_apply(sim_t* s, int i) {
    switch (op_kind(i)) {
    case OP_INSERT:
        for (int k = 0; k < BATCH; k++) s->live[s->next_id++] = 1;
        break;
    case OP_DELETE: {
        uint64_t t[2];
        int n = delete_targets(s, t);
        for (int k = 0; k < n; k++) s->live[t[k]] = 0;
        break;
    }
    default:
        break; /* compaction does not change the live set */
    }
}

static void sim_after(sim_t* s, int ops) {
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < ops; i++) sim_apply(s, i);
}

/* Child: run operations until killed or MAX_OPS; ack each on fd. */
static void child(const char* dir, int fd) {
    lisa_store_t* st = NULL;
    if (lisa_store_open(dir, LISA_STORE_WRITE, MODEL, &st) != LISA_STORE_OK) _exit(3);

    static sim_t sim;
    memset(&sim, 0, sizeof(sim));
    float v[BATCH * DIM];
    lisa_store_chunk_t c[BATCH];

    for (int i = 0; i < MAX_OPS; i++) {
        int rc = LISA_STORE_OK;
        switch (op_kind(i)) {
        case OP_INSERT:
            for (int k = 0; k < BATCH; k++) {
                vec_for(sim.next_id + (uint64_t)k, v + k * DIM);
                c[k].doc_id = (char*)"doc";
                c[k].chunk_index = (int64_t)(sim.next_id + (uint64_t)k);
                c[k].source_path = (char*)"p";
                c[k].offset = 0;
                c[k].length = 0;
                c[k].text = (char*)"t";
                c[k].content_hash = (char*)"h";
                c[k].page = 0;
            }
            rc = lisa_store_insert(st, BATCH, v, c, NULL);
            break;
        case OP_DELETE: {
            uint64_t t[2];
            int n = delete_targets(&sim, t);
            if (n > 0) rc = lisa_store_delete(st, n, t);
            break;
        }
        default:
            rc = lisa_store_compact(st);
            break;
        }
        if (rc != LISA_STORE_OK) _exit(4);
        sim_apply(&sim, i);
        int32_t ack = i;
        if (write(fd, &ack, sizeof(ack)) != (ssize_t)sizeof(ack)) _exit(5);
    }
    lisa_store_close(st);
    _exit(0);
}

static int same_live_set(const lisa_store_view_t* v, const sim_t* s) {
    int64_t sim_count = 0;
    for (uint64_t id = 0; id < s->next_id; id++) sim_count += s->live[id];
    int64_t live = 0;
    for (int64_t i = 0; i < v->n_slots; i++) {
        if (!v->live[i]) continue;
        live++;
        uint64_t id = v->slot_ids[i];
        if (id >= s->next_id || !s->live[id]) return 0;
    }
    return live == sim_count;
}

static int trial(const char* scratch, int t, unsigned seed) {
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/crash_%lld_%d", scratch, (long long)lisa_time_monotonic_ns(), t);
    if (lisa_store_create(dir, MODEL, DIM) != LISA_STORE_OK) {
        printf("  FAIL: trial %d: create\n", t);
        return 1;
    }

    int p[2];
    if (pipe(p) != 0) return 1;
    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        close(p[0]);
        child(dir, p[1]);
    }
    close(p[1]);

    srand(seed);
    lisa_sleep_ms(1 + rand() % 151); /* 1-151 ms */
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);

    int32_t ack, last = -1;
    while (read(p[0], &ack, sizeof(ack)) == (ssize_t)sizeof(ack)) last = ack;
    close(p[0]);

    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        printf("  FAIL: trial %d: child failed with exit %d after op %d\n",
               t, WEXITSTATUS(status), last);
        return 1;
    }

    /* Reopen as the writer: lock must be free, stale files cleaned. */
    lisa_store_t* st = NULL;
    int rc = lisa_store_open(dir, LISA_STORE_WRITE, MODEL, &st);
    if (rc != LISA_STORE_OK) {
        printf("  FAIL: trial %d: reopen after kill at op %d -> %d\n", t, last, rc);
        return 1;
    }
    lisa_store_view_t v;
    if (lisa_store_view(st, &v) != LISA_STORE_OK) {
        printf("  FAIL: trial %d: view\n", t);
        lisa_store_close(st);
        return 1;
    }

    static sim_t a, b;
    sim_after(&a, last + 1);  /* state after the last acknowledged op */
    sim_after(&b, last + 2);  /* ... or after the next one, if it committed */
    int match = same_live_set(&v, &a) ? 1 : (same_live_set(&v, &b) ? 2 : 0);
    if (!match) {
        printf("  FAIL: trial %d: live set matches neither op %d nor op %d\n",
               t, last, last + 1);
        lisa_store_close(st);
        return 1;
    }

    /* Every live chunk's vector and metadata are intact. */
    float want[DIM], got[DIM];
    for (int64_t i = 0; i < v.n_slots; i++) {
        if (!v.live[i]) continue;
        uint64_t id = v.slot_ids[i];
        vec_for(id, want);
        if (memcmp(v.vectors + i * DIM, want, sizeof(want)) != 0 ||
            lisa_store_get_vector(st, id, got) != LISA_STORE_OK ||
            memcmp(got, want, sizeof(want)) != 0) {
            printf("  FAIL: trial %d: vector for id %llu corrupt\n", t, (unsigned long long)id);
            lisa_store_close(st);
            return 1;
        }
        lisa_store_chunk_t c;
        if (lisa_store_get(st, id, &c) != LISA_STORE_OK || c.chunk_index != (int64_t)id) {
            printf("  FAIL: trial %d: metadata for id %llu\n", t, (unsigned long long)id);
            lisa_store_chunk_free(&c);
            lisa_store_close(st);
            return 1;
        }
        lisa_store_chunk_free(&c);
    }

    /* The collection keeps working after recovery. */
    float nv[DIM];
    vec_for(999999, nv);
    lisa_store_chunk_t nc = { (char*)"after", 0, (char*)"", 0, 0, (char*)"", (char*)"", 0 };
    rc = lisa_store_insert(st, 1, nv, &nc, NULL);
    if (rc == LISA_STORE_OK) rc = lisa_store_compact(st);
    lisa_store_close(st);
    if (rc != LISA_STORE_OK) {
        printf("  FAIL: trial %d: write after recovery -> %d\n", t, rc);
        return 1;
    }

    printf("  PASS: trial %d: killed after op %d (%s), state = after op %d\n",
           t, last, last >= 0 ? (op_kind(last + 1) == OP_INSERT ? "next: insert" :
                                 op_kind(last + 1) == OP_DELETE ? "next: delete" : "next: compact")
                              : "before first commit",
           match == 1 ? last : last + 1);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <scratch_dir> [trials]\n", argv[0]);
        return 2;
    }
    int trials = argc > 2 ? atoi(argv[2]) : 40;
    lisa_mkdir(argv[1]);
    printf("Storage crash tests (%d trials)\n", trials);

    int fail = 0;
    for (int t = 0; t < trials; t++) fail |= trial(argv[1], t, 7919u * (unsigned)t + 17u);

    if (fail) {
        printf("FAIL: storage crash tests\n");
        return 1;
    }
    printf("PASS: storage crash tests\n");
    return 0;
}
