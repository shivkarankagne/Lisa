/* SPDX-License-Identifier: BUSL-1.1 */
/*
 * test_documents — extraction, normalisation, and chunking (W5).
 *
 * Usage: test_documents <scratch_dir> <fixtures_dir>
 *
 * PDF tests run when the build has PDFium (LISA_HAVE_PDFIUM); otherwise
 * they are reported as ignored.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "../src/documents/documents.h"
#include "../src/platform/platform.h"

static const char* g_scratch;
static const char* g_fixtures;
static char g_path[1024];
static int g_seq;

void setUp(void) {}
void tearDown(void) {}

/* Write bytes to a fresh scratch file with the given extension. */
static const char* write_file(const char* ext, const char* data, size_t n) {
    snprintf(g_path, sizeof(g_path), "%s/doc_%d.%s", g_scratch, g_seq++, ext);
    FILE* f = fopen(g_path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_size_t(n, fwrite(data, 1, n, f));
    fclose(f);
    return g_path;
}

static void normalize_str(const char* raw, size_t n, char** out) {
    int64_t len = 0;
    TEST_ASSERT_EQUAL_INT(DOC_OK, doc_normalize(raw, (int64_t)n, out, &len));
    TEST_ASSERT_EQUAL_INT64((int64_t)strlen(*out), len);
}

/* ---- normalisation ---------------------------------------------------- */

static void test_normalize_line_endings_and_controls(void) {
    char* o = NULL;
    const char in[] = "a\r\nb\rc\nd\x01\x7f" "e\tf\fg";
    normalize_str(in, sizeof(in) - 1, &o);
    TEST_ASSERT_EQUAL_STRING("a\nb\nc\nde\tf\n\ng", o);
    free(o);
}

static void test_normalize_encodings(void) {
    char* o = NULL;
    /* UTF-8 with BOM */
    normalize_str("\xEF\xBB\xBFhello", 8, &o);
    TEST_ASSERT_EQUAL_STRING("hello", o);
    free(o);
    /* UTF-16LE with BOM: "hé" */
    normalize_str("\xFF\xFEh\x00\xE9\x00", 6, &o);
    TEST_ASSERT_EQUAL_STRING("h\xC3\xA9", o);
    free(o);
    /* UTF-16BE with BOM: "€" */
    normalize_str("\xFE\xFF\x20\xAC", 4, &o);
    TEST_ASSERT_EQUAL_STRING("\xE2\x82\xAC", o);
    free(o);
    /* Invalid UTF-8 -> treated as Windows-1252: 0x93 0x94 are curly quotes, 0xE9 é */
    normalize_str("\x93quoted\x94 caf\xE9", 13, &o);
    TEST_ASSERT_EQUAL_STRING("\xE2\x80\x9Cquoted\xE2\x80\x9D caf\xC3\xA9", o);
    free(o);
}

static void test_normalize_nfc(void) {
    char* a = NULL;
    char* b = NULL;
    normalize_str("e\xCC\x81", 3, &a);   /* e + combining acute */
    normalize_str("\xC3\xA9", 2, &b);    /* precomposed é */
    TEST_ASSERT_EQUAL_STRING(b, a);
    free(a);
    free(b);
    /* Devanagari passes through unchanged. */
    const char* hi = "नमस्ते दुनिया";
    normalize_str(hi, strlen(hi), &a);
    TEST_ASSERT_EQUAL_STRING(hi, a);
    free(a);
}

/* ---- registry and plain text ------------------------------------------ */

static void test_registry(void) {
    TEST_ASSERT_EQUAL_STRING("text", doc_find_extractor("/a/b/notes.TXT")->name);
    TEST_ASSERT_EQUAL_STRING("markdown", doc_find_extractor("README.md")->name);
    TEST_ASSERT_NULL(doc_find_extractor("archive.zip"));
    TEST_ASSERT_EQUAL_STRING("docx", doc_find_extractor("Rent Agreement.DOCX")->name);
    TEST_ASSERT_NULL(doc_find_extractor("old.doc"));
    TEST_ASSERT_NULL(doc_find_extractor("noextension"));
    TEST_ASSERT_NULL(doc_find_extractor("dir.md/file"));
    TEST_ASSERT_NULL(doc_find_extractor("trailingdot."));
#ifdef LISA_HAVE_PDFIUM
    TEST_ASSERT_EQUAL_STRING("pdf", doc_find_extractor("report.Pdf")->name);
#endif
    doc_text_t t;
    TEST_ASSERT_EQUAL_INT(DOC_EUNSUPPORTED, doc_extract("x.zip", &t));
    TEST_ASSERT_NULL(t.text);
}

static void test_text_file(void) {
    const char data[] = "First line.\r\nSecond line.\r\n";
    const char* p = write_file("txt", data, sizeof(data) - 1);
    doc_text_t t;
    TEST_ASSERT_EQUAL_INT(DOC_OK, doc_extract(p, &t));
    TEST_ASSERT_EQUAL_STRING("First line.\nSecond line.\n", t.text);
    TEST_ASSERT_EQUAL_INT64(0, t.n_pages);
    TEST_ASSERT_NULL(t.title);
    TEST_ASSERT_EQUAL_INT64(0, doc_page_at(&t, 5));
    doc_text_free(&t);

    char missing[1100];
    snprintf(missing, sizeof(missing), "%s/missing.txt", g_scratch);
    TEST_ASSERT_EQUAL_INT(DOC_ENOTFOUND, doc_extract(missing, &t));
}

/* ---- markdown --------------------------------------------------------- */

static void test_markdown(void) {
    const char md[] =
        "# Pump Manual\n"
        "\n"
        "Inspect **bearings** every *500* hours. See [the guide](http://x.example).\n"
        "\n"
        "## Steps\n"
        "\n"
        "1. Log the reading\n"
        "2. Stop the pump &amp; notify\n"
        "\n"
        "| Part | Interval |\n"
        "|------|----------|\n"
        "| Bearing | 500 h |\n"
        "\n"
        "<div>html block</div>\n"
        "\n"
        "```\ncode line\n```\n"
        "Inline <b>tag</b> text.\n";
    const char* p = write_file("md", md, sizeof(md) - 1);
    doc_text_t t;
    TEST_ASSERT_EQUAL_INT(DOC_OK, doc_extract(p, &t));
    TEST_ASSERT_EQUAL_STRING("Pump Manual", t.title);
    TEST_ASSERT_NOT_NULL(strstr(t.text, "Pump Manual\n\nInspect bearings every 500 hours. See the guide."));
    TEST_ASSERT_NOT_NULL(strstr(t.text, "- Log the reading\n- Stop the pump & notify"));
    TEST_ASSERT_NOT_NULL(strstr(t.text, "Part | Interval\nBearing | 500 h"));
    TEST_ASSERT_NOT_NULL(strstr(t.text, "code line"));
    TEST_ASSERT_NOT_NULL(strstr(t.text, "Inline tag text."));
    TEST_ASSERT_NULL(strstr(t.text, "**"));
    TEST_ASSERT_NULL(strstr(t.text, "http://x.example"));
    TEST_ASSERT_NULL(strstr(t.text, "<b>"));
    TEST_ASSERT_NULL(strstr(t.text, "\n\n\n"));
    doc_text_free(&t);
}

/* ---- chunking --------------------------------------------------------- */

static int64_t chars_in(const char* s, int64_t n) {
    int64_t c = 0;
    for (int64_t i = 0; i < n; i++) c += ((unsigned char)s[i] & 0xC0) != 0x80;
    return c;
}

static int is_ws(char c) {
    return c == ' ' || c == '\n' || c == '\t';
}

/* Check every guarantee in documents.h for one text and parameter set. */
static void check_chunks(const doc_text_t* t, const doc_chunk_params_t* p) {
    doc_chunk_t* c = NULL;
    int64_t n = 0;
    TEST_ASSERT_EQUAL_INT(DOC_OK, doc_chunk(t, p, &c, &n));
    uint8_t* covered = calloc((size_t)t->len + 1, 1);
    for (int64_t i = 0; i < n; i++) {
        const char* s = t->text + c[i].offset;
        TEST_ASSERT_TRUE(c[i].length > 0);
        TEST_ASSERT_TRUE(c[i].offset + c[i].length <= t->len);
        TEST_ASSERT_TRUE(chars_in(s, c[i].length) <= p->max_chars);
        /* character boundaries, no surrounding whitespace */
        TEST_ASSERT_TRUE(((unsigned char)s[0] & 0xC0) != 0x80);
        if (c[i].offset + c[i].length < t->len)
            TEST_ASSERT_TRUE(((unsigned char)s[c[i].length] & 0xC0) != 0x80);
        TEST_ASSERT_FALSE(is_ws(s[0]));
        TEST_ASSERT_FALSE(is_ws(s[c[i].length - 1]));
        if (i > 0) TEST_ASSERT_TRUE(c[i].offset > c[i - 1].offset);
        TEST_ASSERT_EQUAL_INT64(doc_page_at(t, c[i].offset), c[i].page);
        memset(covered + c[i].offset, 1, (size_t)c[i].length);
    }
    for (int64_t i = 0; i < t->len; i++) {
        if (!is_ws(t->text[i])) TEST_ASSERT_TRUE_MESSAGE(covered[i], "non-whitespace byte not in any chunk");
    }
    free(covered);
    free(c);
}

static doc_text_t make_text(const char* s) {
    doc_text_t t;
    memset(&t, 0, sizeof(t));
    t.text = strdup(s);
    t.len = (int64_t)strlen(s);
    return t;
}

static void test_chunk_small_and_empty(void) {
    doc_chunk_params_t p = DOC_CHUNK_PARAMS_DEFAULT;
    doc_text_t t = make_text("   \n\n\t  ");
    doc_chunk_t* c = NULL;
    int64_t n = -1;
    TEST_ASSERT_EQUAL_INT(DOC_OK, doc_chunk(&t, &p, &c, &n));
    TEST_ASSERT_EQUAL_INT64(0, n);
    doc_text_free(&t);

    t = make_text("\n  Just one short paragraph.  \n");
    TEST_ASSERT_EQUAL_INT(DOC_OK, doc_chunk(&t, &p, &c, &n));
    TEST_ASSERT_EQUAL_INT64(1, n);
    TEST_ASSERT_EQUAL_INT64(3, c[0].offset);
    TEST_ASSERT_EQUAL_INT64(25, c[0].length);  /* "Just one short paragraph." */
    free(c);
    doc_text_free(&t);

    doc_chunk_params_t bad = { 100, 50, 10 };
    TEST_ASSERT_EQUAL_INT(DOC_EINVAL, doc_chunk(&t, &bad, &c, &n));
    doc_chunk_params_t bad2 = { 100, 200, 100 };
    TEST_ASSERT_EQUAL_INT(DOC_EINVAL, doc_chunk(&t, &bad2, &c, &n));
}

static void test_chunk_packs_paragraphs_with_overlap(void) {
    /* 20 paragraphs of ~120 chars each. */
    static char buf[8192];
    buf[0] = '\0';
    for (int i = 0; i < 20; i++) {
        char para[200];
        snprintf(para, sizeof(para),
                 "Paragraph %02d talks about pumps, bearings, vibration limits and the "
                 "steps an operator should follow afterwards.\n\n", i);
        strcat(buf, para);
    }
    doc_text_t t = make_text(buf);
    doc_chunk_params_t p = { 400, 600, 80 };
    check_chunks(&t, &p);

    doc_chunk_t* c = NULL;
    int64_t n = 0;
    TEST_ASSERT_EQUAL_INT(DOC_OK, doc_chunk(&t, &p, &c, &n));
    TEST_ASSERT_TRUE(n >= 5 && n <= 10);
    /* Chunks (after the first) begin inside the previous chunk: overlap. */
    for (int64_t i = 1; i < n; i++)
        TEST_ASSERT_TRUE(c[i].offset < c[i - 1].offset + c[i - 1].length);
    free(c);
    doc_text_free(&t);
}

static void test_chunk_long_paragraph_splits_on_sentences(void) {
    static char buf[20000];
    buf[0] = '\0';
    for (int i = 0; i < 60; i++) strcat(buf, "This sentence is about forty-five chars long. ");
    for (int i = 0; i < 40; i++) strcat(buf, "यह वाक्य हिंदी में लिखा गया है। ");
    doc_text_t t = make_text(buf);
    doc_chunk_params_t p = { 300, 450, 50 };
    check_chunks(&t, &p);

    /* Chunks end at sentence ends (".", "।") when the text allows it. */
    doc_chunk_t* c = NULL;
    int64_t n = 0;
    TEST_ASSERT_EQUAL_INT(DOC_OK, doc_chunk(&t, &p, &c, &n));
    for (int64_t i = 0; i < n; i++) {
        const char* end = t.text + c[i].offset + c[i].length;
        int dot = end[-1] == '.';
        int danda = memcmp(end - 3, "\xE0\xA5\xA4", 3) == 0;  /* U+0964 */
        TEST_ASSERT_TRUE_MESSAGE(dot || danda, "chunk does not end at a sentence end");
    }
    free(c);
    doc_text_free(&t);
}

static void test_chunk_unbreakable_text(void) {
    /* 5000 chars with no spaces or sentence ends: hard cuts at max. */
    static char buf[5001];
    memset(buf, 'x', 5000);
    buf[5000] = '\0';
    doc_text_t t = make_text(buf);
    doc_chunk_params_t p = { 1000, 1500, 150 };
    check_chunks(&t, &p);
    doc_text_free(&t);
}

static void test_chunk_pages(void) {
    doc_text_t t = make_text("Page one text.\n\nPage two text.\n\nPage three text.");
    int64_t offs[3] = { 0, 16, 32 };
    t.n_pages = 3;
    t.page_offsets = malloc(sizeof(offs));
    memcpy(t.page_offsets, offs, sizeof(offs));
    TEST_ASSERT_EQUAL_INT64(1, doc_page_at(&t, 0));
    TEST_ASSERT_EQUAL_INT64(2, doc_page_at(&t, 20));
    TEST_ASSERT_EQUAL_INT64(3, doc_page_at(&t, 40));
    doc_chunk_params_t p = { 20, 30, 0 };
    check_chunks(&t, &p);
    doc_chunk_t* c = NULL;
    int64_t n = 0;
    TEST_ASSERT_EQUAL_INT(DOC_OK, doc_chunk(&t, &p, &c, &n));
    TEST_ASSERT_EQUAL_INT64(3, n);
    TEST_ASSERT_EQUAL_INT64(1, c[0].page);
    TEST_ASSERT_EQUAL_INT64(2, c[1].page);
    TEST_ASSERT_EQUAL_INT64(3, c[2].page);
    free(c);
    doc_text_free(&t);
}

/* Randomised: many shapes of text, every guarantee checked. */
static void test_chunk_randomised(void) {
    static const char* words[] = {
        "pump", "bearing", "vibration", "operator.", "हिंदी", "पाठ।", "données", "\n",
        "\n\n", "limit", "40Hz", "stop?", "रिपोर्ट", "a", "extraordinarilylongword",
    };
    static char buf[40000];
    srand(7);
    for (int trial = 0; trial < 200; trial++) {
        size_t len = 0;
        int n = rand() % 1500;
        for (int i = 0; i < n; i++) {
            const char* w = words[rand() % (sizeof(words) / sizeof(words[0]))];
            size_t wl = strlen(w);
            if (len + wl + 2 >= sizeof(buf)) break;
            memcpy(buf + len, w, wl);
            len += wl;
            if (rand() % 3) buf[len++] = ' ';
        }
        buf[len] = '\0';
        doc_text_t t = make_text(buf);
        int64_t target = 20 + rand() % 400;
        doc_chunk_params_t p = { target, target + rand() % 300, rand() % (target / 2 + 1) };
        if (p.overlap_chars >= p.target_chars) p.overlap_chars = 0;
        check_chunks(&t, &p);
        doc_text_free(&t);
    }
}

/* ---- PDF -------------------------------------------------------------- */

static void test_pdf(void) {
#ifndef LISA_HAVE_PDFIUM
    TEST_IGNORE_MESSAGE("built without PDFium");
#else
    char path[1100];
    snprintf(path, sizeof(path), "%s/two_pages.pdf", g_fixtures);
    doc_text_t t;
    TEST_ASSERT_EQUAL_INT(DOC_OK, doc_extract(path, &t));
    TEST_ASSERT_EQUAL_INT64(2, t.n_pages);
    TEST_ASSERT_EQUAL_STRING("LISA Test Manual", t.title);
    TEST_ASSERT_NOT_NULL(strstr(t.text, "Pump bearings must be inspected every 500 hours."));
    TEST_ASSERT_NOT_NULL(strstr(t.text, "Vibration above 40 Hz indicates wear."));
    int64_t p2 = strstr(t.text, "Vibration") - t.text;
    TEST_ASSERT_EQUAL_INT64(2, doc_page_at(&t, p2));
    TEST_ASSERT_EQUAL_INT64(1, doc_page_at(&t, 0));
    doc_chunk_params_t p = DOC_CHUNK_PARAMS_DEFAULT;
    check_chunks(&t, &p);
    doc_text_free(&t);

    /* Not a PDF, missing file. */
    const char* bad = write_file("pdf", "not a pdf at all", 16);
    TEST_ASSERT_EQUAL_INT(DOC_EFORMAT, doc_extract(bad, &t));
    snprintf(path, sizeof(path), "%s/missing.pdf", g_scratch);
    TEST_ASSERT_EQUAL_INT(DOC_ENOTFOUND, doc_extract(path, &t));
#endif
}

/* ---- Word ------------------------------------------------------------- */

static void test_docx(void) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/sample.docx", g_fixtures);
    doc_text_t t;
    TEST_ASSERT_EQUAL_INT(DOC_OK, doc_extract(path, &t));
    TEST_ASSERT_EQUAL_STRING("Sample Rent Agreement", t.title);
    TEST_ASSERT_EQUAL_INT64(0, t.n_pages);
    /* Runs split mid-number are joined; entities decoded; tabs and breaks kept. */
    TEST_ASSERT_NOT_NULL(strstr(t.text, "monthly rent of Rs. 3,000 (Rupees Three Thousand only) per month."));
    TEST_ASSERT_NOT_NULL(strstr(t.text, "Deposit\tRs. 6,000\nNotice period: one month"));
    TEST_ASSERT_NOT_NULL(strstr(t.text, "Terms & conditions apply <see clause 4>."));
    TEST_ASSERT_NOT_NULL(strstr(t.text, "Maintenance\tRs. 500"));
    TEST_ASSERT_NOT_NULL(strstr(t.text, "\xe0\xa4\x95\xe0\xa4\xbf\xe0\xa4\xb0\xe0\xa4\xbe\xe0\xa4\xaf\xe0\xa4\xbe"));
    TEST_ASSERT_NULL(strstr(t.text, "deleted sentence"));           /* tracked deletion */
    /* Paragraphs are separated by blank lines, so the chunker sees them. */
    TEST_ASSERT_NOT_NULL(strstr(t.text, "RENT AGREEMENT\n\nThe Tenant"));
    doc_chunk_params_t p = DOC_CHUNK_PARAMS_DEFAULT;
    check_chunks(&t, &p);
    doc_text_free(&t);

    /* Not a zip; a zip without word/document.xml; a password-protected file (OLE). */
    const char* bad = write_file("docx", "PK but not really", 17);
    TEST_ASSERT_EQUAL_INT(DOC_EFORMAT, doc_extract(bad, &t));
    TEST_ASSERT_NULL(t.text);
    static const unsigned char ole[16] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
    bad = write_file("docx", (const char*)ole, sizeof(ole));
    TEST_ASSERT_EQUAL_INT(DOC_EENCRYPTED, doc_extract(bad, &t));
    snprintf(path, sizeof(path), "%s/two_pages.pdf", g_fixtures);   /* any non-zip bytes */
    FILE* f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    char buf[64];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    bad = write_file("docx", buf, n);
    TEST_ASSERT_EQUAL_INT(DOC_EFORMAT, doc_extract(bad, &t));
    snprintf(path, sizeof(path), "%s/missing.docx", g_scratch);
    TEST_ASSERT_EQUAL_INT(DOC_ENOTFOUND, doc_extract(path, &t));
}

/* ---- scanned PDF (OCR) ---------------------------------------------------- */

static void test_scanned_pdf_ocr(void) {
#ifndef LISA_HAVE_PDFIUM
    TEST_IGNORE_MESSAGE("built without PDFium");
#else
    char path[1200];
    snprintf(path, sizeof(path), "%s/scanned.pdf", g_fixtures);
    doc_text_t t;
    TEST_ASSERT_EQUAL_INT(DOC_OK, doc_extract(path, &t));
    TEST_ASSERT_EQUAL_INT64(2, t.n_pages);
    if (!lisa_ocr_available()) {
        /* No recogniser on this platform: the scanned pages yield no
         * readable text (only page separators), so none of the words are
         * extracted. */
        TEST_ASSERT_NULL(strstr(t.text, "RENT AGREEMENT"));
        TEST_ASSERT_NULL(strstr(t.text, "Rs. 2,000"));
        doc_text_free(&t);
        TEST_IGNORE_MESSAGE("no OCR on this platform");
    }
    const char* rent = strstr(t.text, "monthly rent of");
    TEST_ASSERT_NOT_NULL_MESSAGE(rent, t.text);
    TEST_ASSERT_NOT_NULL(strstr(t.text, "Rs. 2,000"));
    TEST_ASSERT_NOT_NULL(strstr(t.text, "RENT AGREEMENT"));
    /* Recognised text keeps its page. */
    TEST_ASSERT_EQUAL_INT64(2, doc_page_at(&t, rent - t.text));
    TEST_ASSERT_EQUAL_INT64(1, doc_page_at(&t, strstr(t.text, "RENT AGREEMENT") - t.text));
    doc_text_free(&t);

    /* A page with a real text layer is not sent to OCR (its text is exact). */
    snprintf(path, sizeof(path), "%s/two_pages.pdf", g_fixtures);
    TEST_ASSERT_EQUAL_INT(DOC_OK, doc_extract(path, &t));
    TEST_ASSERT_NOT_NULL(strstr(t.text, "Pump bearings must be inspected every 500 hours."));
    doc_text_free(&t);
#endif
}

static void test_ocr_platform(void) {
    if (!lisa_ocr_available()) TEST_IGNORE_MESSAGE("no OCR on this platform");
    char* out = (char*)1;
    TEST_ASSERT_EQUAL_INT(LISA_PLAT_EINVAL, lisa_ocr_image(NULL, 10, 10, 40, &out));
    TEST_ASSERT_NULL(out);
    unsigned char px[4 * 8 * 8];
    TEST_ASSERT_EQUAL_INT(LISA_PLAT_EINVAL, lisa_ocr_image(px, 8, 8, 16, &out));   /* stride too small */
    memset(px, 0xFF, sizeof(px));
    TEST_ASSERT_EQUAL_INT(LISA_PLAT_OK, lisa_ocr_image(px, 8, 8, 32, &out));      /* blank: no text */
    TEST_ASSERT_EQUAL_STRING("", out);
    free(out);
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <scratch_dir> <fixtures_dir>\n", argv[0]);
        return 2;
    }
    g_scratch = argv[1];
    g_fixtures = argv[2];
    lisa_mkdir(g_scratch);  /* may already exist */
    UNITY_BEGIN();
    RUN_TEST(test_normalize_line_endings_and_controls);
    RUN_TEST(test_normalize_encodings);
    RUN_TEST(test_normalize_nfc);
    RUN_TEST(test_registry);
    RUN_TEST(test_text_file);
    RUN_TEST(test_markdown);
    RUN_TEST(test_chunk_small_and_empty);
    RUN_TEST(test_chunk_packs_paragraphs_with_overlap);
    RUN_TEST(test_chunk_long_paragraph_splits_on_sentences);
    RUN_TEST(test_chunk_unbreakable_text);
    RUN_TEST(test_chunk_pages);
    RUN_TEST(test_chunk_randomised);
    RUN_TEST(test_pdf);
    RUN_TEST(test_docx);
    RUN_TEST(test_scanned_pdf_ocr);
    RUN_TEST(test_ocr_platform);
    return UNITY_END() == 0 ? 0 : 1;
}
