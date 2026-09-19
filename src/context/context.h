/* SPDX-License-Identifier: BUSL-1.1 */
#ifndef LISA_CONTEXT_H
#define LISA_CONTEXT_H
/*
 * Context engine: turns retrieved passages into a prompt that fits a token
 * budget, and reads citations back out of the answer.
 *
 * Work is a pipeline of named stages over a passage list:
 *
 *     retrieve -> filter -> rank -> dedupe -> budget -> generate
 *
 * This module provides filter, rank, dedupe and budget. retrieve and
 * generate need a collection and a model, so the API layer supplies them
 * as stages of its own. A new stage (a reranker, query rewriting) is one
 * more entry in the stage list; callers do not change.
 *
 * No model or storage calls happen here: token counting is a callback,
 * so every stage is testable without a model.
 */

#include <stddef.h>
#include <stdint.h>

#define CTX_OK         0
#define CTX_EINVAL    -1
#define CTX_ENOMEM    -2
#define CTX_ECOUNT    -3   /* the token counter failed */
#define CTX_ETOOLONG  -4   /* the question alone does not fit the budget */

/* The reply the model is told to give when the passages lack the answer. */
#define CTX_NOT_FOUND_TEXT "I could not find this in your documents."

/* One retrieved chunk. Strings are borrowed from the caller. */
typedef struct {
    uint64_t    id;
    const char* doc_id;
    const char* source_path;
    const char* title;         /* may be NULL */
    int64_t     page;          /* 0 if unpaged */
    int64_t     offset;
    int64_t     length;
    const char* text;
    double      score;         /* fused retrieval score; higher is better */
    float       similarity;    /* cosine similarity to the question, -1..1 */
} ctx_passage_t;

/* Count the tokens of a complete prompt (system + user message). */
typedef int (*ctx_count_fn)(void* user, const char* system, const char* user_msg, int64_t* out);

typedef struct {
    float        min_similarity;  /* filter: drop passages below this */
    int64_t      budget_tokens;   /* budget: max prompt tokens */
    ctx_count_fn count;           /* required by the budget stage */
    void*        count_user;
} ctx_params_t;

typedef struct ctx_state {
    const char*    question;
    ctx_params_t   params;
    ctx_passage_t* passages;       /* caller-owned array; stages reorder and shrink it */
    int64_t        n;              /* passages in use */
    int64_t        considered;     /* passages before filtering (set by retrieve) */
    const char*    system;         /* set by budget: the system prompt (static) */
    char*          user_msg;       /* set by budget: malloc'd; ctx_state_clear frees it */
    int64_t        prompt_tokens;  /* set by budget */
    void*          user;           /* for caller-supplied stages */
} ctx_state_t;

typedef int (*ctx_stage_fn)(ctx_state_t* st);

typedef struct {
    const char*  name;
    ctx_stage_fn run;
} ctx_stage_t;

/*
 * Run stages in order; stop at the first error. *failed (may be NULL)
 * gets the failing stage's name. A stage leaving n == 0 does not stop
 * the pipeline: later stages decide what "nothing found" means.
 */
int ctx_run(const ctx_stage_t* stages, int64_t count, ctx_state_t* st, const char** failed);

void ctx_state_clear(ctx_state_t* st);

/* Keep passages with similarity >= params.min_similarity (order kept). */
int ctx_stage_filter(ctx_state_t* st);

/* Order by score, best first (stable). */
int ctx_stage_rank(ctx_state_t* st);

/*
 * Drop a passage whose text equals a better one's, or that overlaps a
 * better passage of the same document by more than half of the shorter
 * one (chunks overlap by design).
 */
int ctx_stage_dedupe(ctx_state_t* st);

/*
 * Keep the best passages whose complete prompt fits params.budget_tokens,
 * skipping any that would overflow it, and build the prompt. Passages are
 * numbered [1]..[n] in the prompt in their final order. If no passage
 * fits, n becomes 0 and no prompt is built.
 */
int ctx_stage_budget(ctx_state_t* st);

/* The default stages this module owns, in order. */
extern const ctx_stage_t ctx_default_stages[4];

/*
 * Passage numbers cited in answer as [n], [n, m] or [n][m], in order of
 * first citation, without duplicates, limited to 1..n_passages. Returns
 * how many were written to out (at most cap).
 */
int64_t ctx_parse_citations(const char* answer, int64_t n_passages, int32_t* out, int64_t cap);

/* 1 if answer is the not-found reply (ignoring surrounding space). */
int ctx_is_not_found(const char* answer);

#endif /* LISA_CONTEXT_H */
