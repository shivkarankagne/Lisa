/* SPDX-License-Identifier: BUSL-1.1 */
#ifndef LISA_DOCUMENTS_H
#define LISA_DOCUMENTS_H

/*
 * LISA documents — turn files into normalised text and chunks.
 *
 * Extraction: each supported format is one extractor, found by file
 * extension through a registry (plan §7 seam: new formats and OCR are new
 * registry entries). Every extractor produces the same thing: UTF-8 text
 * in Unicode NFC, with "\n" line endings, no NUL or other control
 * characters except "\n" and "\t", paragraphs separated by a blank line,
 * and (for paged formats) the byte offset where each page starts.
 *
 * Offsets everywhere in this module are byte offsets into that
 * normalised text, never into the raw file.
 *
 * Chunking: splits normalised text into overlapping chunks that respect
 * paragraph and page boundaries where possible.
 *
 * Thread safety: extraction of PDF files must not run on more than one
 * thread at a time (the PDF engine is not thread-safe). Everything else
 * is re-entrant.
 */

#include <stdint.h>

#define DOC_OK            0
#define DOC_EINVAL       -1
#define DOC_ENOTFOUND    -2   /* no such file */
#define DOC_EIO          -3   /* cannot read the file */
#define DOC_EUNSUPPORTED -4   /* no extractor for this file type */
#define DOC_EFORMAT      -5   /* file is corrupt or not what its extension says */
#define DOC_EENCRYPTED   -6   /* password-protected */
#define DOC_ETOOBIG      -7   /* larger than DOC_MAX_FILE_BYTES */
#define DOC_ENOMEM       -8

#define DOC_MAX_FILE_BYTES (512LL * 1024 * 1024)

typedef struct {
    char*    text;          /* normalised UTF-8, NUL-terminated */
    int64_t  len;           /* bytes, excluding the NUL */
    int64_t  n_pages;       /* 0 for unpaged formats */
    int64_t* page_offsets;  /* n_pages entries, ascending; page i starts at page_offsets[i] */
    char*    title;         /* from the document if it has one, else NULL */
} doc_text_t;

void doc_text_free(doc_text_t* t);

/* 1-based page containing byte offset, or 0 for unpaged text. */
int64_t doc_page_at(const doc_text_t* t, int64_t offset);

/* ---- extraction ------------------------------------------------------- */

typedef int (*doc_extract_fn)(const char* path, doc_text_t* out);

typedef struct {
    const char*        name;        /* "text", "markdown", "pdf" */
    const char* const* extensions;  /* lowercase, without dot; NULL-terminated */
    doc_extract_fn     extract;
} doc_extractor_t;

/* Extractor for path's extension (case-insensitive), or NULL. */
const doc_extractor_t* doc_find_extractor(const char* path);

/* All registered extractors, terminated by an entry with name == NULL. */
const doc_extractor_t* doc_extractors(void);

/* Extract path with its registered extractor. out is zeroed on error. */
int doc_extract(const char* path, doc_text_t* out);

/*
 * Normalise raw bytes of unknown encoding into doc text: UTF-8 (with or
 * without BOM), UTF-16 LE/BE with BOM, otherwise Windows-1252. Output
 * follows the rules in the header comment. *out is malloc'd; *out_len
 * receives its length. Used by extractors; exposed for tests.
 */
int doc_normalize(const char* raw, int64_t raw_len, char** out, int64_t* out_len);

/* ---- chunking --------------------------------------------------------- */

typedef struct {
    int64_t target_chars;   /* aim for chunks of about this many characters */
    int64_t max_chars;      /* hard upper limit per chunk (>= target_chars) */
    int64_t overlap_chars;  /* characters repeated at the start of the next chunk */
} doc_chunk_params_t;

/* target 1000, max 1500, overlap 150 characters (Unicode code points). */
#define DOC_CHUNK_PARAMS_DEFAULT { 1000, 1500, 150 }

typedef struct {
    int64_t offset;  /* byte offset into the normalised text */
    int64_t length;  /* bytes */
    int64_t page;    /* 1-based page where the chunk starts; 0 if unpaged */
} doc_chunk_t;

/*
 * Split t into chunks. *out (malloc'd) receives *n_out chunks in text
 * order. Chunks start and end on UTF-8 character boundaries, contain no
 * leading or trailing whitespace, never exceed max_chars characters, and
 * together cover every non-whitespace character of the text. Text with
 * no non-whitespace characters yields zero chunks.
 */
int doc_chunk(const doc_text_t* t, const doc_chunk_params_t* p,
              doc_chunk_t** out, int64_t* n_out);

#endif /* LISA_DOCUMENTS_H */
