#include "bboxes.h"
#include "bboxes_types.h"

#include <nlohmann/json.hpp>
#include "md5.h"

#include <string>
#include <string_view>

using json = nlohmann::json;

/* ── cursor implementation ──────────────────────────────────────────── */

struct bboxes_cursor {
    BBoxResult result;

    /* doc (single row) */
    bboxes_doc  doc_view;
    std::string doc_json;
    bool        doc_returned;

    /* page iterator */
    size_t      page_index;
    bboxes_page page_view;
    std::string page_json;

    /* font iterator */
    size_t      font_index;
    bboxes_font font_view;
    std::string font_json;

    /* style iterator */
    size_t       style_index;
    bboxes_style style_view;
    std::string  style_json;

    /* bbox iterator (flat across all pages) */
    size_t      bbox_page;    /* which page we're iterating */
    size_t      bbox_within;  /* index within that page's bboxes */
    bboxes_bbox bbox_view;
    std::string bbox_json;
};

/* ── helper: wrap a BBoxResult into a cursor ───────────────────────── */

static bboxes_cursor* wrap_result(BBoxResult r, const void* buf, size_t len) {
    if (r.page_count < 0) return nullptr;
    {
        MD5 md5;
        r.checksum = md5(buf, len);
    }
    auto* c = new bboxes_cursor{};
    c->result       = std::move(r);
    c->doc_returned = false;
    c->page_index   = 0;
    c->font_index   = 0;
    c->style_index  = 0;
    c->bbox_page    = 0;
    c->bbox_within  = 0;
    return c;
}

/* ── format detection ──────────────────────────────────────────────── */

const char* bboxes_detect(const void* buf, size_t len) {
    auto* p = static_cast<const uint8_t*>(buf);
    if (len >= 4 && p[0] == '%' && p[1] == 'P' && p[2] == 'D' && p[3] == 'F')
        return "pdf";
    if (len >= 4 && p[0] == 'P' && p[1] == 'K' && p[2] == 3 && p[3] == 4) {
        /* ZIP archive — scan first 4KB for xl/ or word/ to distinguish */
        size_t scan = len < 4096 ? len : 4096;
        for (size_t i = 0; i + 3 < scan; i++) {
            if (p[i] == 'x' && p[i+1] == 'l' && p[i+2] == '/') return "xlsx";
            if (i + 4 < scan && p[i] == 'w' && p[i+1] == 'o' &&
                p[i+2] == 'r' && p[i+3] == 'd' && p[i+4] == '/') return "docx";
        }
        return "xlsx"; /* default ZIP → xlsx */
    }
    return "text";
}

/* ── auto-detecting open ──────────────────────────────────────────── */

bboxes_cursor* bboxes_open(const void* buf, size_t len) {
    std::string_view fmt = bboxes_detect(buf, len);
    if (fmt == "pdf")  return bboxes_open_pdf(buf, len, nullptr, 0, 0);
    if (fmt == "xlsx") return bboxes_open_xlsx(buf, len, nullptr, 0, 0);
    if (fmt == "docx") return bboxes_open_docx(buf, len);
    return bboxes_open_text(buf, len);
}

/* ── open (PDF backend) ─────────────────────────────────────────────── */

bboxes_cursor* bboxes_open_pdf(const void* buf, size_t len,
                                const char* password,
                                int start_page, int end_page) {
    return wrap_result(extract_pdf(buf, len, password, start_page, end_page), buf, len);
}

/* ── open (XLSX backend) ────────────────────────────────────────────── */

#ifdef BBOXES_HAS_XLSX
bboxes_cursor* bboxes_open_xlsx(const void* buf, size_t len,
                                 const char* password,
                                 int start_page, int end_page) {
    return wrap_result(extract_xlsx(buf, len, password, start_page, end_page), buf, len);
}
#else
void bboxes_xlsx_init(void) {}
void bboxes_xlsx_destroy(void) {}
bboxes_cursor* bboxes_open_xlsx(const void*, size_t, const char*, int, int) {
    return nullptr;
}
#endif

/* ── open (text backend) ────────────────────────────────────────────── */

#ifdef BBOXES_HAS_TEXT
bboxes_cursor* bboxes_open_text(const void* buf, size_t len) {
    return wrap_result(extract_text(buf, len), buf, len);
}
#else
bboxes_cursor* bboxes_open_text(const void*, size_t) { return nullptr; }
#endif

/* ── open (DOCX backend) ────────────────────────────────────────────── */

#ifdef BBOXES_HAS_DOCX
bboxes_cursor* bboxes_open_docx(const void* buf, size_t len) {
    return wrap_result(extract_docx(buf, len), buf, len);
}
#else
bboxes_cursor* bboxes_open_docx(const void*, size_t) { return nullptr; }
#endif

/* ── doc ────────────────────────────────────────────────────────────── */

const bboxes_doc* bboxes_get_doc(bboxes_cursor* c) {
    if (!c) return nullptr;
    c->doc_view.document_id = 0;
    c->doc_view.source_type = c->result.source_type.c_str();
    c->doc_view.filename    = nullptr;
    c->doc_view.checksum    = c->result.checksum.c_str();
    c->doc_view.page_count  = c->result.page_count;
    return &c->doc_view;
}

const char* bboxes_get_doc_json(bboxes_cursor* c) {
    if (!c) return nullptr;
    json obj;
    obj["document_id"] = 0;
    obj["source_type"] = c->result.source_type;
    obj["filename"]    = nullptr;
    obj["checksum"]    = c->result.checksum;
    obj["page_count"]  = c->result.page_count;
    c->doc_json = obj.dump();
    return c->doc_json.c_str();
}

/* ── page iterator ──────────────────────────────────────────────────── */

const bboxes_page* bboxes_next_page(bboxes_cursor* c) {
    if (!c || c->page_index >= c->result.pages.size()) return nullptr;
    const Page& p = c->result.pages[c->page_index++];
    c->page_view.page_id     = p.page_id;
    c->page_view.document_id = p.document_id;
    c->page_view.page_number = p.page_number;
    c->page_view.width       = p.width;
    c->page_view.height      = p.height;
    return &c->page_view;
}

const char* bboxes_next_page_json(bboxes_cursor* c) {
    if (!c || c->page_index >= c->result.pages.size()) return nullptr;
    const Page& p = c->result.pages[c->page_index++];
    json obj;
    obj["page_id"]     = p.page_id;
    obj["document_id"] = p.document_id;
    obj["page_number"] = p.page_number;
    obj["width"]       = p.width;
    obj["height"]      = p.height;
    c->page_json = obj.dump();
    return c->page_json.c_str();
}

/* ── font iterator ──────────────────────────────────────────────────── */

const bboxes_font* bboxes_next_font(bboxes_cursor* c) {
    if (!c || c->font_index >= c->result.fonts.entries.size()) return nullptr;
    const auto& e = c->result.fonts.entries[c->font_index++];
    c->font_view.font_id = e.id;
    c->font_view.name    = e.name.c_str();
    return &c->font_view;
}

const char* bboxes_next_font_json(bboxes_cursor* c) {
    if (!c || c->font_index >= c->result.fonts.entries.size()) return nullptr;
    const auto& e = c->result.fonts.entries[c->font_index++];
    json obj;
    obj["font_id"] = e.id;
    obj["name"]    = e.name;
    c->font_json = obj.dump();
    return c->font_json.c_str();
}

/* ── style iterator ─────────────────────────────────────────────────── */

const bboxes_style* bboxes_next_style(bboxes_cursor* c) {
    if (!c || c->style_index >= c->result.styles.entries.size()) return nullptr;
    const auto& e = c->result.styles.entries[c->style_index++];
    c->style_view.style_id  = e.id;
    c->style_view.font_id   = e.font_id;
    c->style_view.font_size = e.font_size;
    c->style_view.color     = e.color.c_str();
    c->style_view.weight    = e.weight.c_str();
    c->style_view.italic    = e.italic ? 1 : 0;
    c->style_view.underline = e.underline ? 1 : 0;
    return &c->style_view;
}

const char* bboxes_next_style_json(bboxes_cursor* c) {
    if (!c || c->style_index >= c->result.styles.entries.size()) return nullptr;
    const auto& e = c->result.styles.entries[c->style_index++];
    json obj;
    obj["style_id"]  = e.id;
    obj["font_id"]   = e.font_id;
    obj["font_size"] = e.font_size;
    obj["color"]     = e.color;
    obj["weight"]    = e.weight;
    obj["italic"]    = e.italic ? 1 : 0;
    obj["underline"] = e.underline ? 1 : 0;
    c->style_json = obj.dump();
    return c->style_json.c_str();
}

/* ── bbox iterator (flat across all pages) ──────────────────────────── */

const bboxes_bbox* bboxes_next_bbox(bboxes_cursor* c) {
    if (!c) return nullptr;
    while (c->bbox_page < c->result.pages.size()) {
        const auto& page = c->result.pages[c->bbox_page];
        if (c->bbox_within < page.bboxes.size()) {
            const BBox& b = page.bboxes[c->bbox_within++];
            c->bbox_view.page_id  = b.page_id;
            c->bbox_view.style_id = b.style_id;
            c->bbox_view.x = b.x;
            c->bbox_view.y = b.y;
            c->bbox_view.w = b.w;
            c->bbox_view.h = b.h;
            c->bbox_view.text = b.text.c_str();
            c->bbox_view.formula = (c->result.source_type == "xlsx" && !b.formula.empty())
                                   ? b.formula.c_str() : nullptr;
            return &c->bbox_view;
        }
        c->bbox_page++;
        c->bbox_within = 0;
    }
    return nullptr;
}

const char* bboxes_next_bbox_json(bboxes_cursor* c) {
    if (!c) return nullptr;
    while (c->bbox_page < c->result.pages.size()) {
        const auto& page = c->result.pages[c->bbox_page];
        if (c->bbox_within < page.bboxes.size()) {
            const BBox& b = page.bboxes[c->bbox_within++];
            json obj;
            obj["page_id"]  = b.page_id;
            obj["style_id"] = b.style_id;
            obj["x"] = b.x;
            obj["y"] = b.y;
            obj["w"] = b.w;
            obj["h"] = b.h;
            obj["text"] = b.text;
            if (c->result.source_type == "xlsx")
                obj["formula"] = b.formula.empty() ? json(nullptr) : json(b.formula);
            c->bbox_json = obj.dump();
            return c->bbox_json.c_str();
        }
        c->bbox_page++;
        c->bbox_within = 0;
    }
    return nullptr;
}

/* ── close ──────────────────────────────────────────────────────────── */

void bboxes_close(bboxes_cursor* c) {
    delete c;
}
