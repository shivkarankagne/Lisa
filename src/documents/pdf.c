/* SPDX-License-Identifier: Apache-2.0 */
/*
 * PDF extractor (PDFium, statically linked).
 *
 * Text is taken from each page's text layer in reading order as PDFium
 * reports it. Scanned pages have no text layer and contribute nothing
 * (OCR is plan item L3). Pages are separated by a paragraph break and
 * their start offsets recorded, so chunks can cite page numbers.
 *
 * PDFium is not thread-safe: callers must not extract PDFs on more than
 * one thread at a time (documents.h).
 */

#include "doc_internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "fpdf_doc.h"
#include "fpdf_text.h"
#include "fpdfview.h"
#include "../platform/platform.h"

static atomic_int g_pdfium_ready = 0;

static void pdfium_init_once(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_pdfium_ready, &expected, 1)) {
        FPDF_LIBRARY_CONFIG cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.version = 2;
        FPDF_InitLibraryWithConfig(&cfg);
        atomic_store(&g_pdfium_ready, 2);
    }
    while (atomic_load(&g_pdfium_ready) != 2) { /* another thread is initialising */ }
}

/* UTF-16LE (as PDFium returns it, without terminator) -> normalised text. */
static int utf16_to_doc(const unsigned short* u16, int64_t units, char** out, int64_t* out_len) {
    int64_t bytes = 2 + units * 2;
    unsigned char* raw = (unsigned char*)malloc((size_t)bytes);
    if (raw == NULL) return DOC_ENOMEM;
    raw[0] = 0xFF;  /* UTF-16LE byte-order mark for doc_normalize */
    raw[1] = 0xFE;
    for (int64_t i = 0; i < units; i++) {
        raw[2 + 2 * i] = (unsigned char)(u16[i] & 0xFF);
        raw[3 + 2 * i] = (unsigned char)(u16[i] >> 8);
    }
    int rc = doc_normalize((const char*)raw, bytes, out, out_len);
    free(raw);
    return rc;
}

static int append(char** buf, int64_t* len, int64_t* cap, const char* s, int64_t n) {
    if (*len + n + 1 > *cap) {
        int64_t c = *cap ? *cap * 2 : 65536;
        while (c < *len + n + 1) c *= 2;
        char* d = (char*)realloc(*buf, (size_t)c);
        if (d == NULL) return DOC_ENOMEM;
        *buf = d;
        *cap = c;
    }
    memcpy(*buf + *len, s, (size_t)n);
    *len += n;
    (*buf)[*len] = '\0';
    return DOC_OK;
}

static char* meta_title(FPDF_DOCUMENT doc) {
    unsigned long n = FPDF_GetMetaText(doc, "Title", NULL, 0);
    if (n <= 2) return NULL;  /* empty: just the UTF-16 terminator */
    unsigned short* u16 = (unsigned short*)malloc(n);
    if (u16 == NULL) return NULL;
    FPDF_GetMetaText(doc, "Title", u16, n);
    char* t = NULL;
    int64_t tl = 0;
    int rc = utf16_to_doc(u16, (int64_t)(n / 2) - 1, &t, &tl);
    free(u16);
    if (rc != DOC_OK || tl == 0) {
        free(t);
        return NULL;
    }
    return t;
}

int doc_extract_pdf(const char* path, doc_text_t* out) {
    int64_t size = lisa_file_size(path);
    if (size == LISA_PLAT_ENOENT) return DOC_ENOTFOUND;
    if (size < 0) return DOC_EIO;
    if (size > DOC_MAX_FILE_BYTES) return DOC_ETOOBIG;

    pdfium_init_once();
    FPDF_DOCUMENT doc = FPDF_LoadDocument(path, NULL);
    if (doc == NULL) {
        unsigned long err = FPDF_GetLastError();
        if (err == FPDF_ERR_PASSWORD || err == FPDF_ERR_SECURITY) return DOC_EENCRYPTED;
        if (err == FPDF_ERR_FILE) return DOC_EIO;
        return DOC_EFORMAT;
    }

    int pages = FPDF_GetPageCount(doc);
    int rc = pages >= 0 ? DOC_OK : DOC_EFORMAT;
    char* text = NULL;
    int64_t len = 0, cap = 0;
    int64_t* offsets = pages > 0 ? (int64_t*)malloc((size_t)pages * sizeof(int64_t)) : NULL;
    if (pages > 0 && offsets == NULL) rc = DOC_ENOMEM;
    if (rc == DOC_OK) rc = append(&text, &len, &cap, "", 0);

    for (int i = 0; rc == DOC_OK && i < pages; i++) {
        if (i > 0) rc = append(&text, &len, &cap, "\n\n", 2);
        if (rc != DOC_OK) break;
        offsets[i] = len;

        FPDF_PAGE page = FPDF_LoadPage(doc, i);
        if (page == NULL) continue;  /* unreadable page: keep numbering, no text */
        FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
        int n = tp ? FPDFText_CountChars(tp) : 0;
        if (n > 0) {
            unsigned short* u16 = (unsigned short*)malloc(((size_t)n + 1) * sizeof(unsigned short));
            if (u16 == NULL) {
                rc = DOC_ENOMEM;
            } else {
                int got = FPDFText_GetText(tp, 0, n, u16);  /* includes the terminator */
                char* pt = NULL;
                int64_t pl = 0;
                if (got > 1) rc = utf16_to_doc(u16, got - 1, &pt, &pl);
                if (rc == DOC_OK && pl > 0) rc = append(&text, &len, &cap, pt, pl);
                free(pt);
                free(u16);
            }
        }
        if (tp) FPDFText_ClosePage(tp);
        FPDF_ClosePage(page);
    }

    char* title = rc == DOC_OK ? meta_title(doc) : NULL;
    FPDF_CloseDocument(doc);
    if (rc != DOC_OK) {
        free(text);
        free(offsets);
        free(title);
        return rc;
    }
    out->text = text;
    out->len = len;
    out->n_pages = pages;
    out->page_offsets = offsets;
    out->title = title;
    return DOC_OK;
}
