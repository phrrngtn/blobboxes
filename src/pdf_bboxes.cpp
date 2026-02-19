#include "pdf_bboxes.h"

#include <fpdfview.h>
#include <fpdf_text.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

/* ── helpers ────────────────────────────────────────────────────────── */

static const char* style_string(int flags) {
    bool italic = (flags >> 6) & 1;
    bool bold   = (flags >> 18) & 1;
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
    return std::fabs(a.top - b.top) < line_height * 0.5;
}

static bool gap_ok(const CharInfo& prev, const CharInfo& cur) {
    return (cur.left - prev.right) < prev.font_size * 0.35;
}

/* ── internal run struct (owns its strings) ─────────────────────────── */

struct Run {
    uint32_t font_id;
    int page;
    double x, y, w, h;
    std::string text;
    std::string color;
    double font_size;
    std::string style;
};

/* Extract runs from a single page. */
static void extract_page_runs(FPDF_DOCUMENT doc, int pi, FontTable& fonts,
                               std::vector<Run>& out) {
    FPDF_PAGE page = FPDF_LoadPage(doc, pi);
    if (!page) return;

    double page_height = FPDF_GetPageHeight(page);
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

        uint32_t fid = fonts.intern(font_name_buf, font_flags);
        chars.push_back({fid, font_size, r, g, b, a, left, tl_y, right, br_y, cp});
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
            Run run;
            run.font_id   = first.font_id;
            run.page      = pi + 1;
            run.x         = run_left;
            run.y         = run_top;
            run.w         = run_right - run_left;
            run.h         = run_bottom - run_top;
            run.text      = std::move(text);
            run.color     = color_string(first.color_r, first.color_g, first.color_b, first.color_a);
            run.font_size = first.font_size;
            run.style     = style_string(fonts.entries[first.font_id].flags);
            out.push_back(std::move(run));
        }

        i = j;
    }

    FPDFText_ClosePage(text_page);
    FPDF_ClosePage(page);
}

static std::string run_to_json(const Run& r) {
    json obj;
    obj["font_id"]   = r.font_id;
    obj["page"]      = r.page;
    obj["x"]         = r.x;
    obj["y"]         = r.y;
    obj["w"]         = r.w;
    obj["h"]         = r.h;
    obj["text"]      = r.text;
    obj["color"]     = r.color;
    obj["font_size"] = r.font_size;
    obj["style"]     = r.style;
    return obj.dump();
}

/* ── public API ─────────────────────────────────────────────────────── */

void pdf_bboxes_init(void)    { FPDF_InitLibrary(); }
void pdf_bboxes_destroy(void) { FPDF_DestroyLibrary(); }

/* ── extract cursor ────────────────────────────────────────────────── */

struct pdf_bboxes_cursor {
    FPDF_DOCUMENT doc;
    FontTable fonts;
    int current_page;
    int end_page;
    std::vector<Run> page_runs;
    size_t run_index;
    pdf_bboxes_run current;   // C-visible view of current run
    std::string current_json; // cached JSON for current run

    void advance() {
        while (current_page <= end_page) {
            page_runs.clear();
            run_index = 0;
            extract_page_runs(doc, current_page++, fonts, page_runs);
            if (!page_runs.empty()) return;
        }
        page_runs.clear();
        run_index = 0;
    }
};

pdf_bboxes_cursor* pdf_bboxes_extract_open(const void* buf, size_t len,
                                            const char* password,
                                            int start_page, int end_page) {
    FPDF_DOCUMENT doc = FPDF_LoadMemDocument(buf, static_cast<int>(len), password);
    if (!doc) return nullptr;

    int total = FPDF_GetPageCount(doc);
    auto* c = new pdf_bboxes_cursor{};
    c->doc = doc;
    c->run_index = 0;
    c->current_page = (start_page >= 1 ? start_page : 1) - 1;
    c->end_page     = (end_page   >= 1 ? end_page : total) - 1;
    if (c->end_page >= total) c->end_page = total - 1;
    c->advance();
    return c;
}

const pdf_bboxes_run* pdf_bboxes_extract_next(pdf_bboxes_cursor* c) {
    if (!c) return nullptr;
    while (c->run_index >= c->page_runs.size()) {
        if (c->current_page > c->end_page) return nullptr;
        c->advance();
    }
    const Run& r = c->page_runs[c->run_index++];
    c->current.font_id   = r.font_id;
    c->current.page      = r.page;
    c->current.x         = r.x;
    c->current.y         = r.y;
    c->current.w         = r.w;
    c->current.h         = r.h;
    c->current.text      = r.text.c_str();
    c->current.color     = r.color.c_str();
    c->current.font_size = r.font_size;
    c->current.style     = r.style.c_str();
    return &c->current;
}

const char* pdf_bboxes_extract_next_json(pdf_bboxes_cursor* c) {
    if (!c) return nullptr;
    while (c->run_index >= c->page_runs.size()) {
        if (c->current_page > c->end_page) return nullptr;
        c->advance();
    }
    c->current_json = run_to_json(c->page_runs[c->run_index++]);
    return c->current_json.c_str();
}

void pdf_bboxes_extract_close(pdf_bboxes_cursor* c) {
    if (!c) return;
    FPDF_CloseDocument(c->doc);
    delete c;
}

/* ── font cursor ───────────────────────────────────────────────────── */

struct FontEntry {
    uint32_t id;
    std::string name;
    int flags;
    std::string style;
};

struct pdf_bboxes_font_cursor {
    std::vector<FontEntry> entries;
    size_t index;
    pdf_bboxes_font current;  // C-visible view
    std::string current_json;
};

pdf_bboxes_font_cursor* pdf_bboxes_fonts_open(const void* buf, size_t len,
                                               const char* password) {
    FPDF_DOCUMENT doc = FPDF_LoadMemDocument(buf, static_cast<int>(len), password);
    if (!doc) return nullptr;

    FontTable fonts;
    int page_count = FPDF_GetPageCount(doc);
    for (int pi = 0; pi < page_count; ++pi) {
        FPDF_PAGE page = FPDF_LoadPage(doc, pi);
        if (!page) continue;
        FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
        if (!tp) { FPDF_ClosePage(page); continue; }
        int cc = FPDFText_CountChars(tp);
        for (int ci = 0; ci < cc; ++ci) {
            char name[256] = {};
            int flags = 0;
            FPDFText_GetFontInfo(tp, ci, name, sizeof(name), &flags);
            fonts.intern(name, flags);
        }
        FPDFText_ClosePage(tp);
        FPDF_ClosePage(page);
    }
    FPDF_CloseDocument(doc);

    auto* c = new pdf_bboxes_font_cursor{};
    c->index = 0;
    for (const auto& e : fonts.entries)
        c->entries.push_back({e.id, e.name, e.flags, style_string(e.flags)});
    return c;
}

const pdf_bboxes_font* pdf_bboxes_fonts_next(pdf_bboxes_font_cursor* c) {
    if (!c || c->index >= c->entries.size()) return nullptr;
    const FontEntry& e = c->entries[c->index++];
    c->current.font_id = e.id;
    c->current.name    = e.name.c_str();
    c->current.flags   = e.flags;
    c->current.style   = e.style.c_str();
    return &c->current;
}

const char* pdf_bboxes_fonts_next_json(pdf_bboxes_font_cursor* c) {
    if (!c || c->index >= c->entries.size()) return nullptr;
    const FontEntry& e = c->entries[c->index++];
    json obj;
    obj["font_id"] = e.id;
    obj["name"]    = e.name;
    obj["flags"]   = e.flags;
    obj["style"]   = e.style;
    c->current_json = obj.dump();
    return c->current_json.c_str();
}

void pdf_bboxes_fonts_close(pdf_bboxes_font_cursor* c) {
    delete c;
}
