/* SPDX-License-Identifier: BUSL-1.1 */
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

/* A page with fewer visible characters than this is treated as scanned. */
#define OCR_MIN_CHARS 16
/* Render scanned pages at this many pixels on the long side for OCR. */
#define OCR_LONG_SIDE 2000
/* Above this share of unmapped glyphs, the text layer is not trustworthy. */
#define OCR_MAX_UNMAPPED_PERCENT 2

static int64_t visible_chars(const char* s, int64_t n) {
    int64_t k = 0;
    for (int64_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c > ' ' && (c & 0xC0) != 0x80) k++;   /* count characters, not bytes */
    }
    return k;
}

/*
 * Recognise the text of a scanned page. No text (OCR unavailable or
 * nothing found) is not an error: *out stays NULL.
 */
static int ocr_page(FPDF_PAGE page, char** out, int64_t* out_len) {
    *out = NULL;
    *out_len = 0;
    double w = FPDF_GetPageWidth(page), h = FPDF_GetPageHeight(page);
    if (w <= 0 || h <= 0) return DOC_OK;
    double scale = OCR_LONG_SIDE / (w > h ? w : h);
    int pw = (int)(w * scale + 0.5), ph = (int)(h * scale + 0.5);
    FPDF_BITMAP bmp = FPDFBitmap_Create(pw, ph, 0);
    if (bmp == NULL) return DOC_ENOMEM;
    FPDFBitmap_FillRect(bmp, 0, 0, pw, ph, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bmp, page, 0, 0, pw, ph, 0, FPDF_ANNOT);
    char* raw = NULL;
    int rc = lisa_ocr_image((const unsigned char*)FPDFBitmap_GetBuffer(bmp), pw, ph,
                            FPDFBitmap_GetStride(bmp), &raw);
    FPDFBitmap_Destroy(bmp);
    if (rc != LISA_PLAT_OK || raw == NULL) {
        free(raw);
        return rc == LISA_PLAT_ENOMEM ? DOC_ENOMEM : DOC_OK;
    }
    rc = doc_normalize(raw, (int64_t)strlen(raw), out, out_len);
    free(raw);
    return rc;
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
                /*
                 * A font without a Unicode mapping makes PDFium emit
                 * U+FFFE for those glyphs: the page looks like text but
                 * words come out broken ("ve moves" for "five moves").
                 * Past a few per cent, recognise the page instead.
                 */
                int32_t unmapped = 0;
                for (int t = 0; t + 1 < got; t++) {
                    if (u16[t] == 0xFFFE || u16[t] == 0xFFFD) unmapped++;
                }
                if (got > 1 && unmapped * 100 > (got - 1) * OCR_MAX_UNMAPPED_PERCENT &&
                    lisa_ocr_available()) {
                    n = 0;   /* fall through to recognition below */
                } else if (got > 1) {
                    rc = utf16_to_doc(u16, got - 1, &pt, &pl);
                }
                if (rc == DOC_OK && visible_chars(pt, pl) >= OCR_MIN_CHARS)
                    rc = append(&text, &len, &cap, pt, pl);
                else if (rc == DOC_OK) {
                    free(pt);
                    pt = NULL;
                    pl = 0;
                }
                free(pt);
                free(u16);
                if (rc == DOC_OK && pl > 0) n = -1;   /* has a text layer: no OCR */
            }
        }
        /* No usable text layer: a scanned page. Recognise it. */
        if (rc == DOC_OK && n >= 0 && lisa_ocr_available()) {
            char* ot = NULL;
            int64_t ol = 0;
            rc = ocr_page(page, &ot, &ol);
            if (rc == DOC_OK && ol > 0) rc = append(&text, &len, &cap, ot, ol);
            free(ot);
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
