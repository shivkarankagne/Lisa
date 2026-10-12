/* SPDX-License-Identifier: BUSL-1.1 */
/* Shared Office Open XML helpers (see ooxml.h). */

#include "ooxml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/platform.h"

void sbuf_put(sbuf_t* b, const char* s, int64_t n) {
    if (b->oom || n < 0) return;
    if (b->len + n + 1 > b->cap) {
        int64_t cap = b->cap ? b->cap : 4096;
        while (b->len + n + 1 > cap) cap *= 2;
        char* d = (char*)realloc(b->data, (size_t)cap);
        if (d == NULL) {
            b->oom = 1;
            return;
        }
        b->data = d;
        b->cap = cap;
    }
    if (n > 0) memcpy(b->data + b->len, s, (size_t)n);
    b->len += n;
    b->data[b->len] = '\0';
}

void sbuf_puts(sbuf_t* b, const char* s) { sbuf_put(b, s, (int64_t)strlen(s)); }

/* Append the UTF-8 encoding of code point cp (invalid ones are dropped). */
static void put_cp(sbuf_t* b, unsigned long cp) {
    char u[4];
    if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return;
    if (cp < 0x80) {
        u[0] = (char)cp;
        sbuf_put(b, u, 1);
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 0x3F));
        sbuf_put(b, u, 2);
    } else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        sbuf_put(b, u, 3);
    } else {
        u[0] = (char)(0xF0 | (cp >> 18));
        u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[3] = (char)(0x80 | (cp & 0x3F));
        sbuf_put(b, u, 4);
    }
}

void sbuf_put_xml_text(sbuf_t* b, const char* s, const char* e) {
    while (s < e) {
        const char* amp = memchr(s, '&', (size_t)(e - s));
        if (amp == NULL) {
            sbuf_put(b, s, e - s);
            return;
        }
        sbuf_put(b, s, amp - s);
        const char* semi = memchr(amp, ';', (size_t)(e - amp));
        if (semi == NULL || semi - amp > 12) {   /* not an entity: keep the '&' */
            sbuf_put(b, "&", 1);
            s = amp + 1;
            continue;
        }
        size_t n = (size_t)(semi - amp - 1);
        const char* name = amp + 1;
        if (n == 3 && memcmp(name, "amp", 3) == 0) sbuf_put(b, "&", 1);
        else if (n == 2 && memcmp(name, "lt", 2) == 0) sbuf_put(b, "<", 1);
        else if (n == 2 && memcmp(name, "gt", 2) == 0) sbuf_put(b, ">", 1);
        else if (n == 4 && memcmp(name, "quot", 4) == 0) sbuf_put(b, "\"", 1);
        else if (n == 4 && memcmp(name, "apos", 4) == 0) sbuf_put(b, "'", 1);
        else if (n >= 2 && name[0] == '#') {
            char tmp[16];
            memcpy(tmp, name + 1, n - 1);
            tmp[n - 1] = '\0';
            char* end = NULL;
            unsigned long cp = (tmp[0] == 'x' || tmp[0] == 'X') ? strtoul(tmp + 1, &end, 16)
                                                                : strtoul(tmp, &end, 10);
            if (end && *end == '\0') put_cp(b, cp);
        }
        s = semi + 1;
    }
}

void sbuf_trim_end(sbuf_t* b) {
    while (b->len > 0 && (b->data[b->len - 1] == ' ' || b->data[b->len - 1] == '\t')) b->data[--b->len] = '\0';
}

size_t xml_tag_name(const char* p, const char* e) {
    size_t n = 0;
    while (p + n < e && p[n] != '>' && p[n] != '/' && p[n] != ' ' && p[n] != '\t' &&
           p[n] != '\n' && p[n] != '\r')
        n++;
    return n;
}

int xml_is(const char* p, size_t n, const char* name) {
    return strlen(name) == n && memcmp(p, name, n) == 0;
}

char* xml_attr(const char* lt, const char* gt, const char* name) {
    size_t nl = strlen(name);
    for (const char* p = lt + 1; p + nl + 2 < gt; p++) {
        /* name preceded by whitespace and followed by =" or =' */
        if ((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' || p[-1] == '\r') &&
            memcmp(p, name, nl) == 0 && p[nl] == '=' && (p[nl + 1] == '"' || p[nl + 1] == '\'')) {
            char q = p[nl + 1];
            const char* v = p + nl + 2;
            const char* end = memchr(v, q, (size_t)(gt - v));
            if (end == NULL) return NULL;
            sbuf_t b = { 0 };
            sbuf_put(&b, "", 0);
            sbuf_put_xml_text(&b, v, end);
            if (b.oom) {
                free(b.data);
                return NULL;
            }
            return b.data;
        }
    }
    return NULL;
}

int ooxml_open(const char* path, mz_zip_archive* z) {
    int64_t size = lisa_file_size(path);
    if (size == LISA_PLAT_ENOENT) return DOC_ENOTFOUND;
    if (size < 0) return DOC_EIO;
    if (size > DOC_MAX_FILE_BYTES) return DOC_ETOOBIG;
    memset(z, 0, sizeof(*z));
    if (mz_zip_reader_init_file(z, path, 0)) return DOC_OK;
    if (mz_zip_get_last_error(z) == MZ_ZIP_FILE_OPEN_FAILED) return DOC_EIO;
    /* A password-protected Office file is not a zip but an OLE compound file. */
    static const unsigned char ole[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
    unsigned char head[8] = { 0 };
    FILE* f = fopen(path, "rb");
    size_t got = f ? fread(head, 1, sizeof(head), f) : 0;
    if (f) fclose(f);
    return got == sizeof(head) && memcmp(head, ole, sizeof(ole)) == 0 ? DOC_EENCRYPTED : DOC_EFORMAT;
}

char* ooxml_part(mz_zip_archive* z, const char* name, int64_t* out_len, int* too_big) {
    if (too_big) *too_big = 0;
    int idx = mz_zip_reader_locate_file(z, name, NULL, 0);
    if (idx < 0) return NULL;
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(z, (mz_uint)idx, &st)) return NULL;
    if ((int64_t)st.m_uncomp_size > OOXML_MAX_PART_BYTES) {
        if (too_big) *too_big = 1;
        return NULL;
    }
    size_t n = 0;
    char* raw = (char*)mz_zip_reader_extract_to_heap(z, (mz_uint)idx, &n, 0);
    if (raw == NULL) return NULL;
    char* s = (char*)malloc(n + 1);
    if (s) {
        memcpy(s, raw, n);
        s[n] = '\0';
    }
    mz_free(raw);
    if (out_len) *out_len = (int64_t)n;
    return s;
}

char* ooxml_title(mz_zip_archive* z) {
    char* x = ooxml_part(z, "docProps/core.xml", NULL, NULL);
    if (x == NULL) return NULL;
    char* title = NULL;
    const char* s = strstr(x, "<dc:title>");
    const char* e = s ? strstr(s + 10, "</dc:title>") : NULL;
    if (s && e) {
        sbuf_t b = { 0 };
        sbuf_put_xml_text(&b, s + 10, e);
        if (!b.oom && b.len > 0) title = b.data;
        else free(b.data);
    }
    free(x);
    return title;
}

/* Join base_dir and a relative target, collapsing "..", without leading "/". */
static char* resolve(const char* base_dir, const char* target) {
    char buf[1024];
    if (target[0] == '/') snprintf(buf, sizeof(buf), "%s", target + 1);
    else snprintf(buf, sizeof(buf), "%s%s", base_dir, target);
    /* collapse "x/../" segments */
    char out[1024];
    size_t ol = 0;
    const char* p = buf;
    while (*p) {
        const char* slash = strchr(p, '/');
        size_t sl = slash ? (size_t)(slash - p) : strlen(p);
        if (sl == 2 && p[0] == '.' && p[1] == '.') {
            if (ol > 0) ol--;                                 /* drop trailing '/' */
            while (ol > 0 && out[ol - 1] != '/') ol--;        /* drop last segment */
        } else if (!(sl == 1 && p[0] == '.') && sl > 0) {
            if (ol + sl + 2 >= sizeof(out)) return NULL;
            memcpy(out + ol, p, sl);
            ol += sl;
            if (slash) out[ol++] = '/';
        }
        p += sl + (slash ? 1 : 0);
    }
    out[ol] = '\0';
    char* r = (char*)malloc(ol + 1);
    if (r) memcpy(r, out, ol + 1);
    return r;
}

char* ooxml_rel_target(const char* rels, const char* id, const char* base_dir) {
    const char* p = rels;
    while ((p = strstr(p, "<Relationship ")) != NULL) {
        const char* gt = strchr(p, '>');
        if (gt == NULL) return NULL;
        char* rid = xml_attr(p, gt, "Id");
        int match = rid && strcmp(rid, id) == 0;
        free(rid);
        if (match) {
            char* t = xml_attr(p, gt, "Target");
            if (t == NULL) return NULL;
            char* r = resolve(base_dir, t);
            free(t);
            return r;
        }
        p = gt + 1;
    }
    return NULL;
}

void pages_add(pages_t* p, const char* raw, int64_t raw_len) {
    if (p->rc != DOC_OK) return;
    char* t = NULL;
    int64_t tl = 0;
    p->rc = doc_normalize(raw ? raw : "", raw ? raw_len : 0, &t, &tl);
    if (p->rc != DOC_OK) return;
    if (p->n == p->ncap) {
        int64_t nc = p->ncap ? p->ncap * 2 : 16;
        int64_t* o = (int64_t*)realloc(p->offsets, (size_t)nc * sizeof(int64_t));
        if (o == NULL) {
            free(t);
            p->rc = DOC_ENOMEM;
            return;
        }
        p->offsets = o;
        p->ncap = nc;
    }
    int64_t need = p->len + 2 + tl + 1;
    if (need > p->cap) {
        int64_t c = p->cap ? p->cap : 65536;
        while (c < need) c *= 2;
        char* d = (char*)realloc(p->text, (size_t)c);
        if (d == NULL) {
            free(t);
            p->rc = DOC_ENOMEM;
            return;
        }
        p->text = d;
        p->cap = c;
    }
    if (p->n > 0) {
        memcpy(p->text + p->len, "\n\n", 2);
        p->len += 2;
    }
    p->offsets[p->n++] = p->len;
    memcpy(p->text + p->len, t, (size_t)tl);
    p->len += tl;
    p->text[p->len] = '\0';
    free(t);
}

int pages_finish(pages_t* p, doc_text_t* out, char* title) {
    if (p->rc == DOC_OK && p->text == NULL) {
        p->text = (char*)calloc(1, 1);
        if (p->text == NULL) p->rc = DOC_ENOMEM;
    }
    if (p->rc != DOC_OK) {
        free(p->text);
        free(p->offsets);
        free(title);
        return p->rc;
    }
    out->text = p->text;
    out->len = p->len;
    out->n_pages = p->n;
    out->page_offsets = p->offsets;
    out->title = title;
    return DOC_OK;
}
