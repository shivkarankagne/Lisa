/* SPDX-License-Identifier: BUSL-1.1 */
#ifndef LISA_DOC_INTERNAL_H
#define LISA_DOC_INTERNAL_H

/* Shared by the files in src/documents/. Not used elsewhere. */

#include "documents.h"

/* Read a whole file (at most DOC_MAX_FILE_BYTES), NUL-terminated. */
int doc_read_file(const char* path, char** out, int64_t* out_len);

/* Extractors (registered in documents.c). */
int doc_extract_text(const char* path, doc_text_t* out);
int doc_extract_markdown(const char* path, doc_text_t* out);
#ifdef LISA_HAVE_PDFIUM
int doc_extract_pdf(const char* path, doc_text_t* out);
#endif

#endif /* LISA_DOC_INTERNAL_H */
