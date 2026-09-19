/* SPDX-License-Identifier: BUSL-1.1 */
#ifndef LISA_APP_JSON_H
#define LISA_APP_JSON_H
/*
 * JSON forms shared by the HTTP API and `lisa ... --json`, so both
 * interfaces describe answers and ingest progress the same way.
 */

#include "yyjson.h"
#include "lisa.h"

/* Add the fields of an answer to object o. */
void app_json_answer(yyjson_mut_doc* doc, yyjson_mut_val* o, const lisa_answer_t* a);

/* Add the counters of an ingest status to object o. */
void app_json_ingest_status(yyjson_mut_doc* doc, yyjson_mut_val* o, const lisa_ingest_status_t* st);

#endif /* LISA_APP_JSON_H */
