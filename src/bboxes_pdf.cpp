#include "bboxes.h"
#include "bboxes_types.h"

#include <fpdfview.h>
#include <fpdf_text.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* ── helpers ────────────────────────────────────────────────────────── */

static std::string color_string(unsigned r, unsigned g, unsigned b, unsigned a) {
    char buf[40];
    snprintf(buf, sizeof(buf), "rgba(%u,%u,%u,%u)", r, g, b, a);
    return buf;
}

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

static bool same_line(const CharInfo& a, const CharInfo& b) {
    double line_height = a.bottom - a.top;
    if (line_height <= 0) line_height = a.font_size;
    return std::fabs(a.top - b.top) < line_height * 0.5;
}

static bool gap_ok(const CharInfo& prev, const CharInfo& cur) {
    return (cur.left - prev.right) < prev.font_size * 0.35;
}

/* ── extract one page ───────────────────────────────────────────────── */

static void extract_page(FPDF_DOCUMENT doc, int pi,
                          FontTable& fonts, StyleTable& styles,
                          Page& out_page, uint32_t& bbox_counter) {
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
            bb.bbox_id  = bbox_counter++;
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

/* ── public backend API ─────────────────────────────────────────────── */

void bboxes_pdf_init(void)    { FPDF_InitLibrary(); }
void bboxes_pdf_destroy(void) { FPDF_DestroyLibrary(); }

BBoxResult extract_pdf(const void* buf, size_t len, const char* password,
                        int start_page, int end_page) {
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

    uint32_t bbox_counter = 0;
    for (int pi = sp; pi <= ep; ++pi) {
        Page page;
        page.page_id     = static_cast<uint32_t>(result.pages.size());
        page.document_id = 0;
        page.page_number = pi + 1;
        page.width       = 0;
        page.height      = 0;
        extract_page(doc, pi, result.fonts, result.styles, page, bbox_counter);
        result.pages.push_back(std::move(page));
    }

    FPDF_CloseDocument(doc);
    return result;
}
