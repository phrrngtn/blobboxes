#include "bboxes.h"
#include "bboxes_types.h"

#include <fpdfview.h>
#include <fpdf_text.h>
#include <fpdf_edit.h>
#include <fpdf_doc.h>
#include <fpdf_catalog.h>

#include <nlohmann/json.hpp>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

/* PDFium is not thread-safe for document operations.  All calls that
   load/parse/close documents or extract text must be serialized.
   This mutex protects the entire extract_page / extract_page_objects
   call chains.  The cost is negligible — PDF extraction is I/O and
   parse-bound, not contention-bound. */
static std::mutex g_pdfium_mutex;

/* ── helpers ────────────────────────────────────────────────────────── */

static void append_codepoint(std::string& s, unsigned int cp) {
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xF0 | (cp >> 18));
        s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

struct CharInfo {
    uint32_t font_id;
    uint32_t style_id;
    double font_size;
    double left, top, right, bottom;
    unsigned int codepoint;
};

static bool same_style(const CharInfo& a, const CharInfo& b) {
    return a.style_id == b.style_id;
}

/* Determine whether two characters are on the same visual line.
   Compares top-edge positions with a tolerance of half the line height.

   The line_height estimate prefers the character's bbox height (bottom - top),
   falling back to font_size.  When font_size is reliable (> 1.0) and larger
   than the bbox height, we use font_size instead — this handles cases where
   the bbox is a partial glyph (e.g. a period or comma whose rendered height
   is much smaller than the font's line height).

   FRAGILE: the 0.5× tolerance assumes single-spaced text.  Superscripts and
   subscripts whose top offset exceeds half the line height will be treated as
   separate lines.  This is usually correct (footnote markers, exponents) but
   will break for PDFs that render fractions as stacked inline glyphs. */
static bool same_line(const CharInfo& a, const CharInfo& b) {
    double line_height = a.bottom - a.top;
    if (line_height <= 0) line_height = a.font_size;
    double fs = a.font_size;
    if (fs > 1.0 && fs > line_height) line_height = fs;
    return std::fabs(a.top - b.top) < line_height * 0.5;
}

/* Conservative character-gap test: should these two adjacent characters
   be part of the same bbox?

   Design decision: this layer is deliberately conservative.  It only
   merges characters that are unambiguously part of the same glyph run
   (inter-character gaps typical of kerning and proportional spacing).
   Anything wider — including normal inter-word spaces — breaks the run
   and starts a new bbox.

   The rationale: too many bboxes is a better failure mode than too few.
   Downstream SQL/Python can always merge adjacent bboxes on the same
   line (it's a linear window-function scan over sorted coordinates).
   But if the C++ layer merges two table cells into one bbox, the column
   boundary is destroyed and can't be recovered without re-parsing text.

   The 0.35× font_size threshold is the original value and matches the
   typical inter-character gap in proportional fonts (0.05–0.25× em).
   Inter-word gaps are typically 0.3–0.5× em, so they will produce a
   new bbox.  This means every word becomes its own bbox — that's fine,
   the downstream coalescing layer has the full spatial context (column
   alignment, row structure) to decide which words belong together.

   FPDFText_GetFontSize returns the raw font dictionary size, which is
   often 1.0 when the actual rendered size comes from the text matrix
   (Tm) rather than the font resource.  This is common in government
   forms (IRS W-4), Adobe InDesign exports, and some LaTeX-generated
   PDFs.  When font_size is 1.0, the 0.35× threshold collapses to
   0.35 points and nothing coalesces — every character becomes its own
   bbox.  Fix: fall back to the character's bbox height (bottom - top),
   which always reflects the rendered size.  We also fall back when
   char_h drastically exceeds font_size (> 1.5×), which happens when
   font_size is a small "design unit" and the text matrix scales it up.

   FRAGILE: if a PDF genuinely has 1pt text (fine-print footnotes), this
   heuristic uses bbox height instead, which should be similar.  But if
   bbox height is inflated by a large descender/ascender on a particular
   glyph, the threshold will be too generous and may merge characters
   that should be separate.  We haven't seen this in practice. */
static bool gap_ok(const CharInfo& prev, const CharInfo& cur) {
    double gap = cur.left - prev.right;
    double fs = prev.font_size;
    double char_h = prev.bottom - prev.top;
    if (char_h > 0 && (fs <= 1.0 || char_h > fs * 1.5))
        fs = char_h;
    return gap < fs * 0.35;
}

/* ── extract one page ───────────────────────────────────────────────── */

static void extract_page(FPDF_DOCUMENT doc, int pi,
                          FontTable& fonts, StyleTable& styles,
                          Page& out_page) {
    FPDF_PAGE page = FPDF_LoadPage(doc, pi);
    if (!page) return;

    out_page.width  = FPDF_GetPageWidth(page);
    out_page.height = FPDF_GetPageHeight(page);

    double page_height = out_page.height;
    FPDF_TEXTPAGE text_page = FPDFText_LoadPage(page);
    if (!text_page) { FPDF_ClosePage(page); return; }

    int char_count = FPDFText_CountChars(text_page);
    std::vector<CharInfo> chars;
    chars.reserve(char_count);

    for (int ci = 0; ci < char_count; ++ci) {
        unsigned int cp = FPDFText_GetUnicode(text_page, ci);
        if (cp == 0 || cp == 0xFFFE || cp == 0xFFFF) continue;

        double left, right, bottom, top;
        if (!FPDFText_GetCharBox(text_page, ci, &left, &right, &bottom, &top)) continue;

        double tl_y = page_height - top;
        double br_y = page_height - bottom;

        char font_name_buf[256] = {};
        int font_flags = 0;
        FPDFText_GetFontInfo(text_page, ci, font_name_buf, sizeof(font_name_buf), &font_flags);
        double font_size = FPDFText_GetFontSize(text_page, ci);

        unsigned int r = 0, g = 0, b = 0, a = 255;
        FPDFText_GetFillColor(text_page, ci, &r, &g, &b, &a);

        uint32_t fid = fonts.intern(font_name_buf);

        bool bold   = (font_flags >> 18) & 1;
        bool italic = (font_flags >> 6) & 1;
        /* Fallback: infer bold/italic from the font name when flags are absent */
        if (!bold || !italic) {
            FontNameTraits traits = font_name_traits(font_name_buf);
            if (!bold)   bold   = traits.bold;
            if (!italic) italic = traits.italic;
        }
        std::string weight = bold ? "bold" : "normal";
        std::string color  = color_string(r, g, b, a);

        uint32_t sid = styles.intern(fid, font_size, color, weight, italic, false);
        chars.push_back({fid, sid, font_size, left, tl_y, right, br_y, cp});
    }

    for (size_t i = 0; i < chars.size(); ) {
        const CharInfo& first = chars[i];

        if (first.codepoint == ' ' || first.codepoint == '\t' ||
            first.codepoint == '\r' || first.codepoint == '\n') {
            ++i;
            continue;
        }

        double run_left = first.left, run_top = first.top;
        double run_right = first.right, run_bottom = first.bottom;
        std::string text;
        append_codepoint(text, first.codepoint);

        size_t j = i + 1;
        while (j < chars.size()) {
            const CharInfo& cur = chars[j];
            if (!same_style(first, cur) || !same_line(first, cur)) break;
            if (!gap_ok(chars[j - 1], cur)) break;
            append_codepoint(text, cur.codepoint);
            if (cur.right  > run_right)  run_right  = cur.right;
            if (cur.bottom > run_bottom) run_bottom = cur.bottom;
            if (cur.left   < run_left)   run_left   = cur.left;
            if (cur.top    < run_top)    run_top    = cur.top;
            ++j;
        }

        while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
            text.pop_back();

        if (!text.empty()) {
            BBox bb;
            bb.page_id  = out_page.page_id;
            bb.style_id = first.style_id;
            bb.x = run_left;
            bb.y = run_top;
            bb.w = run_right - run_left;
            bb.h = run_bottom - run_top;
            bb.text = std::move(text);
            out_page.bboxes.push_back(std::move(bb));
        }

        i = j;
    }

    FPDFText_ClosePage(text_page);
    FPDF_ClosePage(page);
}

/* ── Object-level page extraction ──────────────────────────────────────
   Uses FPDFPage_GetObject / FPDFTextObj_GetText instead of iterating
   characters one at a time.  Each PDF text object is typically a word
   or short phrase — the PDF's own text runs.  This avoids the per-char
   GetCharBox/GetFontInfo/GetFillColor calls and the coalescing loop.

   Trade-off: no sub-word positioning (can't split "Q1Sales" if the PDF
   encodes it as one text object).  But for born-digital PDFs, text
   objects almost always correspond to natural word boundaries. */

static void extract_page_objects(FPDF_DOCUMENT doc, int pi,
                                  FontTable& fonts, StyleTable& styles,
                                  Page& out_page) {
    FPDF_PAGE page = FPDF_LoadPage(doc, pi);
    if (!page) return;

    out_page.width  = FPDF_GetPageWidth(page);
    out_page.height = FPDF_GetPageHeight(page);
    double page_height = out_page.height;

    /* Need a TEXTPAGE for FPDFTextObj_GetText */
    FPDF_TEXTPAGE text_page = FPDFText_LoadPage(page);

    int obj_count = FPDFPage_CountObjects(page);

    for (int oi = 0; oi < obj_count; ++oi) {
        FPDF_PAGEOBJECT obj = FPDFPage_GetObject(page, oi);
        if (FPDFPageObj_GetType(obj) != FPDF_PAGEOBJ_TEXT) continue;

        /* ── Text content ── */
        /* First call with NULL to get required buffer length */
        unsigned long text_len = FPDFTextObj_GetText(obj, text_page, NULL, 0);
        if (text_len <= 2) continue;  /* empty or just null terminator (UTF-16) */

        std::vector<unsigned short> utf16(text_len);
        FPDFTextObj_GetText(obj, text_page, utf16.data(), text_len);

        /* Convert UTF-16 to UTF-8, skip whitespace-only */
        std::string text;
        for (unsigned long k = 0; k < text_len - 1; ++k) {
            unsigned int cp = utf16[k];
            /* Handle surrogate pairs */
            if (cp >= 0xD800 && cp <= 0xDBFF && k + 1 < text_len - 1) {
                unsigned int lo = utf16[k + 1];
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ++k;
                }
            }
            if (cp == 0) break;
            append_codepoint(text, cp);
        }

        /* Trim trailing whitespace */
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t'
                                 || text.back() == '\r' || text.back() == '\n'))
            text.pop_back();
        /* Trim leading whitespace */
        size_t start = 0;
        while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
            ++start;
        if (start > 0) text.erase(0, start);

        if (text.empty()) continue;

        /* ── Bounding box ── */
        float left, bottom, right, top;
        if (!FPDFPageObj_GetBounds(obj, &left, &bottom, &right, &top)) continue;

        double bb_x = static_cast<double>(left);
        double bb_y = page_height - static_cast<double>(top);
        double bb_w = static_cast<double>(right - left);
        double bb_h = static_cast<double>(top - bottom);

        /* ── Font / style ── */
        FPDF_FONT font = FPDFTextObj_GetFont(obj);
        char font_name_buf[256] = {};
        if (font) {
            FPDFFont_GetFamilyName(font, font_name_buf, sizeof(font_name_buf));
        }
        float font_size_f = 0;
        FPDFTextObj_GetFontSize(obj, &font_size_f);
        double font_size = static_cast<double>(font_size_f);
        /* Font size fallback: use bbox height if reported size is bogus */
        if (font_size <= 1.0 && bb_h > 1.0) font_size = bb_h;

        int font_flags = font ? FPDFFont_GetFlags(font) : 0;
        bool bold   = (font_flags >> 18) & 1;
        /* Also check font weight as a fallback for bold detection */
        if (!bold && font) {
            int weight = FPDFFont_GetWeight(font);
            if (weight >= 700) bold = true;
        }
        bool italic = (font_flags >> 6) & 1;
        /* Final fallback: infer bold/italic from the font name */
        if (!bold || !italic) {
            FontNameTraits traits = font_name_traits(font_name_buf);
            if (!bold)   bold   = traits.bold;
            if (!italic) italic = traits.italic;
        }

        unsigned int r = 0, g = 0, b = 0, a = 255;
        FPDFPageObj_GetFillColor(obj, &r, &g, &b, &a);

        uint32_t fid = fonts.intern(font_name_buf);
        std::string weight = bold ? "bold" : "normal";
        std::string color  = color_string(r, g, b, a);
        uint32_t sid = styles.intern(fid, font_size, color, weight, italic, false);

        BBox bb;
        bb.page_id  = out_page.page_id;
        bb.style_id = sid;
        bb.x = bb_x;
        bb.y = bb_y;
        bb.w = bb_w;
        bb.h = bb_h;
        bb.text = std::move(text);
        out_page.bboxes.push_back(std::move(bb));
    }

    if (text_page) FPDFText_ClosePage(text_page);
    FPDF_ClosePage(page);
}

/* ── public backend API ─────────────────────────────────────────────── */

void bboxes_pdf_init(void)    { FPDF_InitLibrary(); }
void bboxes_pdf_destroy(void) { FPDF_DestroyLibrary(); }

BBoxResult extract_pdf(const void* buf, size_t len, const char* password,
                        int start_page, int end_page) {
    std::lock_guard<std::mutex> lock(g_pdfium_mutex);
    BBoxResult result;
    result.source_type = "pdf";

    FPDF_DOCUMENT doc = FPDF_LoadMemDocument(buf, static_cast<int>(len), password);
    if (!doc) {
        result.page_count = -1;
        return result;
    }

    int total = FPDF_GetPageCount(doc);
    result.page_count = total;

    int sp = (start_page >= 1 ? start_page : 1) - 1;
    int ep = (end_page   >= 1 ? end_page : total) - 1;
    if (ep >= total) ep = total - 1;

    for (int pi = sp; pi <= ep; ++pi) {
        Page page;
        page.page_id     = static_cast<uint32_t>(result.pages.size());
        page.document_id = 0;
        page.page_number = pi + 1;
        page.width       = 0;
        page.height      = 0;
        extract_page(doc, pi, result.fonts, result.styles, page);
        result.pages.push_back(std::move(page));
    }

    FPDF_CloseDocument(doc);
    return result;
}

/* ── document metadata (JSON clob) ──────────────────────────────────────
   Full-take PDF metadata: Info dict + structural (version, page_count,
   encryption + permissions, tagged). Shares g_pdfium_mutex and the
   append_codepoint UTF-16 decoder above; assumes bboxes_pdf_init() has
   run (same contract as extract_pdf — no FPDF_InitLibrary here).

   XMP (/Metadata stream) is deliberately deferred — PDFium exposes no
   simple public accessor for the raw stream; it is a later increment. */

using json = nlohmann::json;

/* Thread-local storage backing the returned const char* (valid until the
   next metadata call on the same thread). */
static thread_local std::string g_pdf_meta;

/* FPDF_GetMetaText → UTF-8. Returns "" when the tag is absent. */
static std::string pdf_meta_text(FPDF_DOCUMENT doc, const char* tag) {
    unsigned long n = FPDF_GetMetaText(doc, tag, nullptr, 0);
    if (n <= 2) return {};                       /* just the UTF-16 NUL */
    std::vector<unsigned short> buf(n / 2);
    FPDF_GetMetaText(doc, tag, buf.data(), n);
    std::string out;
    for (size_t k = 0; k < buf.size(); ++k) {
        unsigned int cp = buf[k];
        if (cp == 0) break;
        if (cp >= 0xD800 && cp <= 0xDBFF && k + 1 < buf.size()) {
            unsigned int lo = buf[k + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++k;
            }
        }
        append_codepoint(out, cp);
    }
    return out;
}

/* "D:20211025160858+05'30'" → "2021-10-25T16:08:58+05:30" (lossless).
   Returns the input unchanged if it does not look like a PDF date. */
static std::string pdf_date_iso(const std::string& s) {
    size_t i = (s.size() >= 2 && s[0] == 'D' && s[1] == ':') ? 2 : 0;
    auto grab = [&](int count) {
        std::string r;
        for (int k = 0; k < count && i < s.size() && std::isdigit((unsigned char)s[i]); ++k)
            r += s[i++];
        return r;
    };
    std::string Y = grab(4);
    if (Y.size() < 4) return s;
    auto pad = [](std::string v, const char* def) { return v.size() == 2 ? v : std::string(def); };
    std::string Mo = pad(grab(2), "01"), D = pad(grab(2), "01");
    std::string H = pad(grab(2), "00"), Mi = pad(grab(2), "00"), S = pad(grab(2), "00");
    std::string iso = Y + "-" + Mo + "-" + D + "T" + H + ":" + Mi + ":" + S;
    if (i < s.size()) {
        char o = s[i];
        if (o == 'Z' || o == 'z') {
            iso += "Z";
        } else if (o == '+' || o == '-') {
            ++i;
            std::string oh = grab(2);
            if (i < s.size() && s[i] == '\'') ++i;
            std::string om = grab(2);
            if (oh.size() == 2) iso += std::string(1, o) + oh + ":" + (om.size() == 2 ? om : "00");
        }
    }
    return iso;
}

/* Build the JSON from an opened (or NULL) document. Caller holds the mutex. */
static std::string pdf_meta_build(FPDF_DOCUMENT doc) {
    json out;
    out["dialect"] = "pdf";
    if (!doc) {
        unsigned long e = FPDF_GetLastError();
        const char* reason =
            e == FPDF_ERR_PASSWORD ? "encrypted (password required)" :
            e == FPDF_ERR_FORMAT   ? "not a PDF / corrupted" :
            e == FPDF_ERR_FILE     ? "file not found / unreadable" :
            e == FPDF_ERR_SECURITY ? "unsupported security scheme" : "unknown";
        out["integrity"] = {{"status", e == FPDF_ERR_PASSWORD ? "encrypted" : "failed"},
                            {"error", reason}};
        return out.dump(1);
    }
    out["integrity"] = {{"status", "clean"}};

    /* Info dict */
    static const struct { const char* tag; const char* key; bool date; } kInfo[] = {
        {"Title", "title", false},     {"Author", "author", false},
        {"Subject", "subject", false}, {"Keywords", "keywords", false},
        {"Creator", "creator", false}, {"Producer", "producer", false},
        {"CreationDate", "created", true}, {"ModDate", "modified", true},
        {"Trapped", "trapped", false},
    };
    json info = json::object();
    for (const auto& m : kInfo) {
        std::string v = pdf_meta_text(doc, m.tag);
        if (!v.empty()) info[m.key] = m.date ? pdf_date_iso(v) : v;
    }
    if (!info.empty()) out["info"] = info;

    /* Structural */
    json st = json::object();
    int ver = 0;
    if (FPDF_GetFileVersion(doc, &ver))
        st["version"] = std::to_string(ver / 10) + "." + std::to_string(ver % 10);
    st["page_count"] = FPDF_GetPageCount(doc);
    int rev = FPDF_GetSecurityHandlerRevision(doc);   /* -1 if not encrypted */
    bool encrypted = rev >= 0;
    st["encrypted"] = encrypted;
    if (encrypted) {
        st["security_revision"] = rev;
        /* Permission bits are 1-based in the PDF spec (bit 1 = LSB). */
        unsigned long p = FPDF_GetDocPermissions(doc);
        st["permissions"] = {
            {"print",              (bool)(p & (1u << 2))},
            {"modify",             (bool)(p & (1u << 3))},
            {"copy",               (bool)(p & (1u << 4))},
            {"annotate",           (bool)(p & (1u << 5))},
            {"fill_forms",         (bool)(p & (1u << 8))},
            {"extract_accessible", (bool)(p & (1u << 9))},
            {"assemble",           (bool)(p & (1u << 10))},
            {"print_high_res",     (bool)(p & (1u << 11))},
        };
    }
    st["tagged"] = (bool)FPDFCatalog_IsTagged(doc);
    out["structural"] = st;
    return out.dump(1);
}

/* Streaming block reader for FPDF_LoadCustomDocument — PDFium requests only
   the byte ranges it needs (trailer, xref, referenced objects), so a large
   PDF is never read whole. */
static int pdf_file_getblock(void* param, unsigned long pos,
                             unsigned char* buf, unsigned long size) {
    FILE* fp = static_cast<FILE*>(param);
    if (fseeko(fp, static_cast<off_t>(pos), SEEK_SET) != 0) return 0;
    return fread(buf, 1, size, fp) == size ? 1 : 0;
}

extern "C" {

const char* bboxes_pdf_metadata_json(const void* buf, size_t len) {
    std::lock_guard<std::mutex> lock(g_pdfium_mutex);
    /* FPDF_LoadMemDocument borrows `buf` (no copy); it stays valid for the
       whole call, and we close the doc before returning. */
    FPDF_DOCUMENT doc = FPDF_LoadMemDocument(buf, static_cast<int>(len), nullptr);
    g_pdf_meta = pdf_meta_build(doc);
    if (doc) FPDF_CloseDocument(doc);
    return g_pdf_meta.c_str();
}

const char* bboxes_pdf_metadata_json_file(const char* path) {
    std::lock_guard<std::mutex> lock(g_pdfium_mutex);
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        FPDF_DOCUMENT none = nullptr;
        (void)none;
        json out;
        out["dialect"] = "pdf";
        out["integrity"] = {{"status", "failed"}, {"error", "file not found / unreadable"}};
        g_pdf_meta = out.dump(1);
        return g_pdf_meta.c_str();
    }
    fseeko(fp, 0, SEEK_END);
    off_t sz = ftello(fp);
    fseeko(fp, 0, SEEK_SET);

    FPDF_FILEACCESS fa{};
    fa.m_FileLen = static_cast<unsigned long>(sz);
    fa.m_GetBlock = pdf_file_getblock;
    fa.m_Param = fp;                     /* kept alive on the stack below */

    FPDF_DOCUMENT doc = FPDF_LoadCustomDocument(&fa, nullptr);
    g_pdf_meta = pdf_meta_build(doc);
    if (doc) FPDF_CloseDocument(doc);
    fclose(fp);
    return g_pdf_meta.c_str();
}

} /* extern "C" */

/* Object-level variant — uses FPDFPage_GetObject instead of char-by-char */
BBoxResult extract_pdf_objects(const void* buf, size_t len, const char* password,
                                int start_page, int end_page) {
    std::lock_guard<std::mutex> lock(g_pdfium_mutex);
    BBoxResult result;
    result.source_type = "pdf";

    FPDF_DOCUMENT doc = FPDF_LoadMemDocument(buf, static_cast<int>(len), password);
    if (!doc) {
        result.page_count = -1;
        return result;
    }

    int total = FPDF_GetPageCount(doc);
    result.page_count = total;

    int sp = (start_page >= 1 ? start_page : 1) - 1;
    int ep = (end_page   >= 1 ? end_page : total) - 1;
    if (ep >= total) ep = total - 1;

    for (int pi = sp; pi <= ep; ++pi) {
        Page page;
        page.page_id     = static_cast<uint32_t>(result.pages.size());
        page.document_id = 0;
        page.page_number = pi + 1;
        page.width       = 0;
        page.height      = 0;
        extract_page_objects(doc, pi, result.fonts, result.styles, page);
        result.pages.push_back(std::move(page));
    }

    FPDF_CloseDocument(doc);
    return result;
}
