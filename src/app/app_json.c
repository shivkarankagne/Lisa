/* SPDX-License-Identifier: BUSL-1.1 */
/* Shared JSON forms (app_json.h). */

#include "app_json.h"

void app_json_answer(yyjson_mut_doc* doc, yyjson_mut_val* o, const lisa_answer_t* a) {
    yyjson_mut_obj_add_strcpy(doc, o, "text", a->text);
    yyjson_mut_obj_add_bool(doc, o, "found", a->found != 0);
    yyjson_mut_obj_add_bool(doc, o, "complete", a->complete != 0);
    yyjson_mut_val* arr = yyjson_mut_obj_add_arr(doc, o, "citations");
    for (int64_t i = 0; i < a->citation_count; i++) {
        const lisa_citation_t* c = &a->citations[i];
        yyjson_mut_val* x = yyjson_mut_arr_add_obj(doc, arr);
        yyjson_mut_obj_add_int(doc, x, "number", c->number);
        yyjson_mut_obj_add_uint(doc, x, "chunk_id", c->chunk_id);
        yyjson_mut_obj_add_strcpy(doc, x, "path", c->source_path);
        yyjson_mut_obj_add_strcpy(doc, x, "title", c->title);
        yyjson_mut_obj_add_int(doc, x, "page", c->page);
        yyjson_mut_obj_add_int(doc, x, "offset", c->offset);
        yyjson_mut_obj_add_int(doc, x, "length", c->length);
        yyjson_mut_obj_add_strcpy(doc, x, "quote", c->quote);
        yyjson_mut_obj_add_strcpy(doc, x, "content_hash", c->content_hash);
        yyjson_mut_obj_add_real(doc, x, "similarity", c->similarity);
    }
    yyjson_mut_obj_add_int(doc, o, "passages_retrieved", a->passages_retrieved);
    yyjson_mut_obj_add_int(doc, o, "passages_used", a->passages_used);
    yyjson_mut_obj_add_int(doc, o, "prompt_tokens", a->prompt_tokens);
    yyjson_mut_obj_add_int(doc, o, "answer_tokens", a->answer_tokens);
    yyjson_mut_obj_add_real(doc, o, "first_token_seconds", a->first_token_seconds);
    yyjson_mut_obj_add_real(doc, o, "total_seconds", a->total_seconds);
}

void app_json_ingest_status(yyjson_mut_doc* doc, yyjson_mut_val* o, const lisa_ingest_status_t* st) {
    yyjson_mut_obj_add_int(doc, o, "files_seen", st->files_seen);
    yyjson_mut_obj_add_int(doc, o, "files_added", st->files_added);
    yyjson_mut_obj_add_int(doc, o, "files_updated", st->files_updated);
    yyjson_mut_obj_add_int(doc, o, "files_unchanged", st->files_unchanged);
    yyjson_mut_obj_add_int(doc, o, "files_no_text", st->files_no_text);
    yyjson_mut_obj_add_int(doc, o, "files_failed", st->files_failed);
    yyjson_mut_obj_add_int(doc, o, "files_removed", st->files_removed);
    yyjson_mut_obj_add_int(doc, o, "files_skipped", st->files_skipped);
    yyjson_mut_obj_add_int(doc, o, "chunks_added", st->chunks_added);
    yyjson_mut_obj_add_int(doc, o, "chunks_removed", st->chunks_removed);
    yyjson_mut_obj_add_real(doc, o, "elapsed_seconds", st->elapsed_seconds);
}
