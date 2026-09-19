/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Markdown extractor: md4c parse events -> plain text with paragraph
 * structure. Markup is removed; the words a reader sees are kept (link
 * text, image alt text, code, table cells). Inline and block HTML are
 * dropped.
 */

#include "doc_internal.h"

#include <stdlib.h>
#include <string.h>

#include "md4c.h"
#include "utf8proc.h"

typedef struct {
    char*   buf;
    int64_t len, cap;
    int     oom;
    int     table_cell;     /* cells emitted in the current row */
    int     in_h1;
    int     title_done;
    char*   title;
    int64_t title_len, title_cap;
} md_out_t;

static void put(md_out_t* o, const char* s, int64_t n) {
    if (o->oom || n <= 0) return;
    if (o->len + n + 1 > o->cap) {
        int64_t cap = o->cap ? o->cap * 2 : 4096;
        while (cap < o->len + n + 1) cap *= 2;
        char* d = (char*)realloc(o->buf, (size_t)cap);
        if (d == NULL) {
            o->oom = 1;
            return;
        }
        o->buf = d;
        o->cap = cap;
    }
    memcpy(o->buf + o->len, s, (size_t)n);
    o->len += n;
    o->buf[o->len] = '\0';

    if (o->in_h1 && !o->title_done) {
        if (o->title_len + n + 1 > o->title_cap) {
            int64_t cap = o->title_cap ? o->title_cap * 2 : 128;
            while (cap < o->title_len + n + 1) cap *= 2;
            char* d = (char*)realloc(o->title, (size_t)cap);
            if (d == NULL) {
                o->oom = 1;
                return;
            }
            o->title = d;
            o->title_cap = cap;
        }
        memcpy(o->title + o->title_len, s, (size_t)n);
        o->title_len += n;
        o->title[o->title_len] = '\0';
    }
}

static void puts_(md_out_t* o, const char* s) {
    put(o, s, (int64_t)strlen(s));
}

/* End the current line / paragraph without piling up blank lines. */
static void newlines(md_out_t* o, int want) {
    int have = 0;
    for (int64_t i = o->len - 1; i >= 0 && o->buf[i] == '\n' && have < want; i--) have++;
    if (o->len == 0) return;
    for (; have < want; have++) put(o, "\n", 1);
}

static int enter_block(MD_BLOCKTYPE type, void* detail, void* user) {
    md_out_t* o = (md_out_t*)user;
    switch (type) {
    case MD_BLOCK_H:
        newlines(o, 2);
        if (((MD_BLOCK_H_DETAIL*)detail)->level == 1 && !o->title_done) o->in_h1 = 1;
        break;
    case MD_BLOCK_P:
    case MD_BLOCK_CODE:
    case MD_BLOCK_QUOTE:
    case MD_BLOCK_TABLE:
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        newlines(o, 2);
        break;
    case MD_BLOCK_LI:
        newlines(o, 1);
        puts_(o, "- ");
        break;
    case MD_BLOCK_TR:
        newlines(o, 1);
        o->table_cell = 0;
        break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        if (o->table_cell++ > 0) puts_(o, " | ");
        break;
    default:
        break;
    }
    return 0;
}

static int leave_block(MD_BLOCKTYPE type, void* detail, void* user) {
    (void)detail;
    md_out_t* o = (md_out_t*)user;
    switch (type) {
    case MD_BLOCK_H:
        if (o->in_h1) {
            o->in_h1 = 0;
            o->title_done = 1;
        }
        newlines(o, 2);
        break;
    case MD_BLOCK_P:
    case MD_BLOCK_CODE:
    case MD_BLOCK_TABLE:
    case MD_BLOCK_HR:
        newlines(o, 2);
        break;
    default:
        break;
    }
    return 0;
}

static int enter_span(MD_SPANTYPE type, void* detail, void* user) {
    (void)type; (void)detail; (void)user;
    return 0;
}

static int leave_span(MD_SPANTYPE type, void* detail, void* user) {
    (void)type; (void)detail; (void)user;
    return 0;
}

/* Decode an HTML entity (&amp; &#233; &#x2014; ...) or emit it verbatim. */
static void entity(md_out_t* o, const MD_CHAR* s, MD_SIZE n) {
    static const struct { const char* name; const char* text; } k[] = {
        { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" },
        { "&apos;", "'" }, { "&nbsp;", "\xC2\xA0" }, { "&mdash;", "\xE2\x80\x94" },
        { "&ndash;", "\xE2\x80\x93" }, { "&hellip;", "\xE2\x80\xA6" },
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (strlen(k[i].name) == n && memcmp(k[i].name, s, n) == 0) {
            puts_(o, k[i].text);
            return;
        }
    }
    if (n >= 4 && s[1] == '#') {
        long cp = (s[2] == 'x' || s[2] == 'X') ? strtol(s + 3, NULL, 16) : strtol(s + 2, NULL, 10);
        if (cp > 0 && utf8proc_codepoint_valid((utf8proc_int32_t)cp)) {
            utf8proc_uint8_t b[4];
            put(o, (const char*)b, (int64_t)utf8proc_encode_char((utf8proc_int32_t)cp, b));
            return;
        }
    }
    put(o, s, (int64_t)n);
}

static int text(MD_TEXTTYPE type, const MD_CHAR* s, MD_SIZE n, void* user) {
    md_out_t* o = (md_out_t*)user;
    switch (type) {
    case MD_TEXT_BR:       put(o, "\n", 1); break;
    case MD_TEXT_SOFTBR:   put(o, " ", 1); break;
    case MD_TEXT_ENTITY:   entity(o, s, n); break;
    case MD_TEXT_HTML:     break;  /* inline HTML tags are markup */
    case MD_TEXT_NULLCHAR: break;
    default:               put(o, s, (int64_t)n); break;
    }
    return 0;
}

int doc_extract_markdown(const char* path, doc_text_t* out) {
    char* raw = NULL;
    int64_t raw_len = 0;
    int rc = doc_read_file(path, &raw, &raw_len);
    if (rc != DOC_OK) return rc;

    /* Parse clean UTF-8 so offsets and encodings are uniform. */
    char* clean = NULL;
    int64_t clean_len = 0;
    rc = doc_normalize(raw, raw_len, &clean, &clean_len);
    free(raw);
    if (rc != DOC_OK) return rc;

    md_out_t o;
    memset(&o, 0, sizeof(o));
    MD_PARSER parser;
    memset(&parser, 0, sizeof(parser));
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB;
    parser.enter_block = enter_block;
    parser.leave_block = leave_block;
    parser.enter_span = enter_span;
    parser.leave_span = leave_span;
    parser.text = text;
    int prc = md_parse(clean, (MD_SIZE)clean_len, &parser, &o);
    free(clean);
    if (o.oom) rc = DOC_ENOMEM;
    else if (prc != 0) rc = DOC_EFORMAT;

    if (rc == DOC_OK) {
        /* Re-normalise: joins of text pieces keep the module's guarantees. */
        rc = doc_normalize(o.buf ? o.buf : "", o.len, &out->text, &out->len);
    }
    if (rc == DOC_OK && o.title_len > 0) {
        out->title = o.title;
        o.title = NULL;
    }
    free(o.buf);
    free(o.title);
    return rc;
}
