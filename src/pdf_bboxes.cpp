#include "pdf_bboxes.h"

#include <fpdfview.h>
#include <fpdf_text.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

/* ── helpers ────────────────────────────────────────────────────────── */

static const char* style_string(int flags) {
    bool italic = (flags >> 6) & 1;   /* bit 7 (0-indexed bit 6) */
    bool bold   = (flags >> 18) & 1;  /* bit 19 (0-indexed bit 18) */
    if (bold && italic) return "bold-italic";
    if (bold)           return "bold";
    if (italic)         return "italic";
    return "normal";
}

struct FontKey {
    std::string name;
    int flags;
    bool operator==(const FontKey& o) const { return name == o.name && flags == o.flags; }
};

struct FontKeyHash {
    size_t operator()(const FontKey& k) const {
        size_t h = std::hash<std::string>{}(k.name);
        h ^= std::hash<int>{}(k.flags) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct FontTable {
    std::unordered_map<FontKey, uint32_t, FontKeyHash> map;
    struct Entry { uint32_t id; std::string name; int flags; };
    std::vector<Entry> entries;

    uint32_t intern(const char* name, int flags) {
        FontKey key{name ? name : "", flags};
        auto it = map.find(key);
        if (it != map.end()) return it->second;
        uint32_t id = static_cast<uint32_t>(entries.size());
        map[key] = id;
        entries.push_back({id, key.name, flags});
        return id;
    }
};

struct CharInfo {
    uint32_t font_id;
    double font_size;
    unsigned int color_r, color_g, color_b, color_a;
    double left, top, right, bottom;
    unsigned int codepoint;
};

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

static bool same_style(const CharInfo& a, const CharInfo& b) {
    return a.font_id == b.font_id &&
           a.font_size == b.font_size &&
           a.color_r == b.color_r && a.color_g == b.color_g &&
           a.color_b == b.color_b && a.color_a == b.color_a;
}

static bool same_line(const CharInfo& a, const CharInfo& b) {
    double line_height = a.bottom - a.top;
    if (line_height <= 0) line_height = a.font_size;
    double dy = std::fabs(a.top - b.top);
    return dy < line_height * 0.5;
}

static bool gap_ok(const CharInfo& prev, const CharInfo& cur) {
    double gap = cur.left - prev.right;
    return gap < prev.font_size * 0.35;
}

/* ── public API ─────────────────────────────────────────────────────── */

void pdf_bboxes_init(void) {
    FPDF_InitLibrary();
}

void pdf_bboxes_destroy(void) {
    FPDF_DestroyLibrary();
}

int pdf_bboxes_extract(const void* buf, size_t len, const char* password,
                       pdf_bbox_callback cb, void* user_data) {
    FPDF_DOCUMENT doc = FPDF_LoadMemDocument(buf, static_cast<int>(len), password);
    if (!doc) return -1;

    FontTable fonts;
    int page_count = FPDF_GetPageCount(doc);

    for (int pi = 0; pi < page_count; ++pi) {
        FPDF_PAGE page = FPDF_LoadPage(doc, pi);
        if (!page) continue;

        double page_height = FPDF_GetPageHeight(page);
        FPDF_TEXTPAGE text_page = FPDFText_LoadPage(page);
        if (!text_page) { FPDF_ClosePage(page); continue; }

        int char_count = FPDFText_CountChars(text_page);

        /* Collect char info */
        std::vector<CharInfo> chars;
        chars.reserve(char_count);

        for (int ci = 0; ci < char_count; ++ci) {
            unsigned int cp = FPDFText_GetUnicode(text_page, ci);
            if (cp == 0 || cp == 0xFFFE || cp == 0xFFFF) continue;
            /* Skip whitespace-only chars (space, tab, CR, LF) for bbox purposes,
               but we will use spaces to decide about gaps. */

            double left, right, bottom, top;
            if (!FPDFText_GetCharBox(text_page, ci, &left, &right, &bottom, &top)) continue;

            /* PDFium gives bottom-up coordinates; convert to top-down */
            double tl_y = page_height - top;
            double br_y = page_height - bottom;

            char font_name_buf[256] = {};
            int font_flags = 0;
            FPDFText_GetFontInfo(text_page, ci, font_name_buf, sizeof(font_name_buf), &font_flags);
            double font_size = FPDFText_GetFontSize(text_page, ci);

            unsigned int r = 0, g = 0, b = 0, a = 255;
            FPDFText_GetFillColor(text_page, ci, &r, &g, &b, &a);

            uint32_t fid = fonts.intern(font_name_buf, font_flags);

            chars.push_back({fid, font_size, r, g, b, a, left, tl_y, right, br_y, cp});
        }

        /* Group into runs */
        for (size_t i = 0; i < chars.size(); ) {
            const CharInfo& first = chars[i];

            /* Skip lone whitespace that starts a run */
            if (first.codepoint == ' ' || first.codepoint == '\t' ||
                first.codepoint == '\r' || first.codepoint == '\n') {
                ++i;
                continue;
            }

            double run_left   = first.left;
            double run_top    = first.top;
            double run_right  = first.right;
            double run_bottom = first.bottom;
            std::string text;
            append_codepoint(text, first.codepoint);

            size_t j = i + 1;
            while (j < chars.size()) {
                const CharInfo& cur = chars[j];
                /* Break on style change or line change */
                if (!same_style(first, cur) || !same_line(first, cur)) break;
                /* Break on large horizontal gap */
                if (!gap_ok(chars[j - 1], cur)) break;

                append_codepoint(text, cur.codepoint);
                if (cur.right  > run_right)  run_right  = cur.right;
                if (cur.bottom > run_bottom) run_bottom = cur.bottom;
                if (cur.left   < run_left)   run_left   = cur.left;
                if (cur.top    < run_top)    run_top    = cur.top;
                ++j;
            }

            /* Trim trailing whitespace from text */
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
                text.pop_back();

            if (!text.empty()) {
                json obj;
                obj["font_id"]   = first.font_id;
                obj["page"]      = pi + 1;
                obj["x"]         = run_left;
                obj["y"]         = run_top;
                obj["w"]         = run_right - run_left;
                obj["h"]         = run_bottom - run_top;
                obj["text"]      = text;
                obj["color"]     = color_string(first.color_r, first.color_g, first.color_b, first.color_a);
                obj["font_size"] = first.font_size;
                obj["style"]     = style_string(fonts.entries[first.font_id].flags);

                std::string s = obj.dump();
                int rc = cb(s.c_str(), user_data);
                if (rc != 0) {
                    FPDFText_ClosePage(text_page);
                    FPDF_ClosePage(page);
                    FPDF_CloseDocument(doc);
                    return rc;
                }
            }

            i = j;
        }

        FPDFText_ClosePage(text_page);
        FPDF_ClosePage(page);
    }

    FPDF_CloseDocument(doc);
    return 0;
}

int pdf_bboxes_fonts(const void* buf, size_t len, const char* password,
                     pdf_bbox_callback cb, void* user_data) {
    FPDF_DOCUMENT doc = FPDF_LoadMemDocument(buf, static_cast<int>(len), password);
    if (!doc) return -1;

    FontTable fonts;
    int page_count = FPDF_GetPageCount(doc);

    /* Scan all chars to discover fonts */
    for (int pi = 0; pi < page_count; ++pi) {
        FPDF_PAGE page = FPDF_LoadPage(doc, pi);
        if (!page) continue;
        FPDF_TEXTPAGE text_page = FPDFText_LoadPage(page);
        if (!text_page) { FPDF_ClosePage(page); continue; }

        int char_count = FPDFText_CountChars(text_page);
        for (int ci = 0; ci < char_count; ++ci) {
            char font_name_buf[256] = {};
            int font_flags = 0;
            FPDFText_GetFontInfo(text_page, ci, font_name_buf, sizeof(font_name_buf), &font_flags);
            fonts.intern(font_name_buf, font_flags);
        }

        FPDFText_ClosePage(text_page);
        FPDF_ClosePage(page);
    }

    FPDF_CloseDocument(doc);

    /* Emit font entries */
    for (const auto& e : fonts.entries) {
        json obj;
        obj["font_id"] = e.id;
        obj["name"]    = e.name;
        obj["flags"]   = e.flags;
        obj["style"]   = style_string(e.flags);

        std::string s = obj.dump();
        int rc = cb(s.c_str(), user_data);
        if (rc != 0) return rc;
    }

    return 0;
}
