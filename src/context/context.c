/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * Context engine stages (context.h). The system prompt was checked on
 * Qwen3-4B (report 009): facts cited as [n], and questions the passages
 * do not answer get exactly CTX_NOT_FOUND_TEXT.
 */

#include "context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char k_system[] =
    "You answer questions using only the numbered passages from the user's documents.\n"
    "Rules:\n"
    "- Use only facts stated in the passages. Never use outside knowledge.\n"
    "- A passage's heading names the document it came from; use it to connect the question to the "
    "passage, and do not refuse because a passage omits a name the question uses.\n"
    "- If passages disagree, give each answer with its own citation and say they differ.\n"
    "- After each fact, cite the passage it came from in square brackets, like [S1].\n"
    "- If the passages do not contain the answer, reply exactly: " CTX_NOT_FOUND_TEXT "\n"
    "- Answer briefly, in the language of the question.";

int ctx_run(const ctx_stage_t* stages, int64_t count, ctx_state_t* st, const char** failed) {
    if (failed) *failed = NULL;
    if (stages == NULL || st == NULL || count < 0) return CTX_EINVAL;
    for (int64_t i = 0; i < count; i++) {
        int rc = stages[i].run(st);
        if (rc != CTX_OK) {
            if (failed) *failed = stages[i].name;
            return rc;
        }
    }
    return CTX_OK;
}

void ctx_state_clear(ctx_state_t* st) {
    if (st == NULL) return;
    free(st->user_msg);
    st->user_msg = NULL;
}

/* ---- filter, rank, dedupe ------------------------------------------- */

int ctx_stage_filter(ctx_state_t* st) {
    int64_t k = 0;
    for (int64_t i = 0; i < st->n; i++) {
        if (st->passages[i].similarity >= st->params.min_similarity) st->passages[k++] = st->passages[i];
    }
    st->n = k;
    return CTX_OK;
}

int ctx_stage_rank(ctx_state_t* st) {
    /* Insertion sort: n is small, and it is stable. */
    for (int64_t i = 1; i < st->n; i++) {
        ctx_passage_t x = st->passages[i];
        int64_t j = i - 1;
        while (j >= 0 && st->passages[j].score < x.score) {
            st->passages[j + 1] = st->passages[j];
            j--;
        }
        st->passages[j + 1] = x;
    }
    return CTX_OK;
}

static int duplicates(const ctx_passage_t* a, const ctx_passage_t* b) {
    if (a->text && b->text && strcmp(a->text, b->text) == 0) return 1;
    if (a->doc_id == NULL || b->doc_id == NULL || strcmp(a->doc_id, b->doc_id) != 0) return 0;
    int64_t lo = a->offset > b->offset ? a->offset : b->offset;
    int64_t hi_a = a->offset + a->length, hi_b = b->offset + b->length;
    int64_t hi = hi_a < hi_b ? hi_a : hi_b;
    int64_t shorter = a->length < b->length ? a->length : b->length;
    return shorter > 0 && hi - lo > shorter / 2;
}

int ctx_stage_dedupe(ctx_state_t* st) {
    int64_t k = 0;
    for (int64_t i = 0; i < st->n; i++) {
        int dup = 0;
        for (int64_t j = 0; j < k && !dup; j++) dup = duplicates(&st->passages[j], &st->passages[i]);
        if (!dup) st->passages[k++] = st->passages[i];
    }
    st->n = k;
    return CTX_OK;
}

/* ---- prompt ----------------------------------------------------------- */

typedef struct {
    char*  data;
    size_t len, cap;
    int    oom;
} buf_t;

static void put(buf_t* b, const char* s, size_t n) {
    if (b->oom) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (b->len + n + 1 > cap) cap *= 2;
        char* d = (char*)realloc(b->data, cap);
        if (d == NULL) {
            b->oom = 1;
            return;
        }
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void puts_(buf_t* b, const char* s) { put(b, s, strlen(s)); }

/*
 * Document text must never become chat-template markup: break up the
 * "<|" and "|>" that delimit special tokens (e.g. <|im_start|>), so a
 * document cannot inject a system or assistant turn.
 */
static void put_text(buf_t* b, const char* s) {
    const char* run = s;
    for (; *s; s++) {
        if ((s[0] == '<' && s[1] == '|') || (s[0] == '|' && s[1] == '>')) {
            put(b, run, (size_t)(s - run + 1));
            put(b, " ", 1);
            run = s + 1;
        }
    }
    put(b, run, (size_t)(s - run));
}

static const char* base_name(const char* path) {
    const char* slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : (path ? path : "");
}

/* Prompt with the passages whose `use` flag is set, numbered in order. */
static char* build_user(const ctx_state_t* st, const uint8_t* use) {
    buf_t b = { 0 };
    int64_t num = 0;
    for (int64_t i = 0; i < st->n; i++) {
        if (!use[i]) continue;
        const ctx_passage_t* p = &st->passages[i];
        if (num == 0) puts_(&b, "Passages:\n\n");
        char head[64];
        snprintf(head, sizeof(head), "[S%lld] (", (long long)++num);
        puts_(&b, head);
        put_text(&b, p->title && p->title[0] ? p->title : base_name(p->source_path));
        if (p->page > 0) {
            snprintf(head, sizeof(head), ", page %lld", (long long)p->page);
            puts_(&b, head);
        }
        puts_(&b, ")\n");
        put_text(&b, p->text ? p->text : "");
        puts_(&b, "\n\n");
    }
    puts_(&b, "Question: ");
    put_text(&b, st->question);
    if (b.oom) {
        free(b.data);
        return NULL;
    }
    return b.data;
}

static int count_with(ctx_state_t* st, const uint8_t* use, char** out_msg, int64_t* out_n) {
    char* msg = build_user(st, use);
    if (msg == NULL) return CTX_ENOMEM;
    if (st->params.count(st->params.count_user, k_system, msg, out_n) != 0) {
        free(msg);
        return CTX_ECOUNT;
    }
    *out_msg = msg;
    return CTX_OK;
}

int ctx_stage_budget(ctx_state_t* st) {
    if (st->question == NULL || st->params.count == NULL || st->params.budget_tokens <= 0) return CTX_EINVAL;
    ctx_state_clear(st);
    st->system = k_system;
    st->prompt_tokens = 0;

    uint8_t* use = (uint8_t*)calloc((size_t)(st->n > 0 ? st->n : 1), 1);
    if (use == NULL) return CTX_ENOMEM;
    char* msg = NULL;
    int64_t tokens = 0;
    int rc = count_with(st, use, &msg, &tokens);
    if (rc == CTX_OK && tokens > st->params.budget_tokens) rc = CTX_ETOOLONG;
    int64_t kept = 0;
    for (int64_t i = 0; rc == CTX_OK && i < st->n; i++) {
        use[i] = 1;
        char* m2 = NULL;
        int64_t t2 = 0;
        rc = count_with(st, use, &m2, &t2);
        if (rc != CTX_OK) break;
        if (t2 <= st->params.budget_tokens) {
            free(msg);
            msg = m2;
            tokens = t2;
            kept++;
        } else {
            use[i] = 0;
            free(m2);
        }
    }
    if (rc == CTX_OK) {
        int64_t k = 0;
        for (int64_t i = 0; i < st->n; i++) {
            if (use[i]) st->passages[k++] = st->passages[i];
        }
        st->n = k;
        if (kept > 0) {
            st->user_msg = msg;
            st->prompt_tokens = tokens;
            msg = NULL;
        }
    }
    free(msg);
    free(use);
    return rc;
}

const ctx_stage_t ctx_default_stages[4] = {
    { "filter", ctx_stage_filter },
    { "rank",   ctx_stage_rank },
    { "dedupe", ctx_stage_dedupe },
    { "budget", ctx_stage_budget },
};

/* ---- reading the answer ----------------------------------------------- */

/*
 * Passages are labelled [S1], [S2]... in the prompt: a document's own
 * numbering ("15. That both parties...") would otherwise be echoed as
 * [15] and point at a passage that does not exist. A bare [1] is still
 * accepted, since models sometimes drop the letter.
 */
int64_t ctx_parse_citations(const char* answer, int64_t n_passages, int32_t* out, int64_t cap) {
    if (answer == NULL || out == NULL || cap <= 0) return 0;
    int64_t n = 0;
    for (const char* s = answer; *s; s++) {
        if (*s != '[') continue;
        /* [S1], [S1, S2], [1]; anything else inside the brackets ends it. */
        const char* p = s + 1;
        int32_t nums[16];
        int k = 0, ok = 0;
        while (1) {
            while (*p == ' ') p++;
            if (*p == 'S' || *p == 's') p++;
            if (*p < '0' || *p > '9') break;
            long v = 0;
            while (*p >= '0' && *p <= '9' && v < 100000) v = v * 10 + (*p++ - '0');
            if (k < 16) nums[k++] = (int32_t)v;
            while (*p == ' ') p++;
            if (*p == ']') {
                ok = 1;
                break;
            }
            if (*p != ',') break;
            p++;
        }
        if (!ok) continue;
        for (int i = 0; i < k; i++) {
            if (nums[i] < 1 || nums[i] > n_passages) continue;
            int seen = 0;
            for (int64_t j = 0; j < n && !seen; j++) seen = out[j] == nums[i];
            if (!seen && n < cap) out[n++] = nums[i];
        }
        s = p;
    }
    return n;
}

int ctx_is_not_found(const char* answer) {
    if (answer == NULL) return 0;
    while (*answer == ' ' || *answer == '\n' || *answer == '\t' || *answer == '\r') answer++;
    size_t n = strlen(CTX_NOT_FOUND_TEXT) - 1;   /* the final '.' is optional */
    if (strncmp(answer, CTX_NOT_FOUND_TEXT, n) != 0) return 0;
    const char* s = answer + n;
    if (*s == '.') s++;
    for (; *s; s++) {
        if (*s != ' ' && *s != '\n' && *s != '\t' && *s != '\r') return 0;
    }
    return 1;
}
