#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include "bboxes.h"
#include <string>
#include <vector>

namespace nb = nanobind;

/* ── helper: cursor → Python dicts (shared by all backends) ──────── */

static nb::dict cursor_doc(bboxes_cursor* cur) {
    auto* d = bboxes_get_doc(cur);
    nb::dict out;
    out["document_id"] = d->document_id;
    out["source_type"] = d->source_type;
    out["filename"]    = d->filename ? nb::cast(d->filename) : nb::none();
    out["checksum"]    = d->checksum;
    out["page_count"]  = d->page_count;
    return out;
}

static nb::list cursor_pages(bboxes_cursor* cur) {
    nb::list out;
    while (auto* p = bboxes_next_page(cur)) {
        nb::dict d;
        d["page_id"]     = p->page_id;
        d["document_id"] = p->document_id;
        d["page_number"] = p->page_number;
        d["width"]       = p->width;
        d["height"]      = p->height;
        out.append(d);
    }
    return out;
}

static nb::list cursor_fonts(bboxes_cursor* cur) {
    nb::list out;
    while (auto* f = bboxes_next_font(cur)) {
        nb::dict d;
        d["font_id"] = f->font_id;
        d["name"]    = f->name;
        out.append(d);
    }
    return out;
}

static nb::list cursor_styles(bboxes_cursor* cur) {
    nb::list out;
    while (auto* s = bboxes_next_style(cur)) {
        nb::dict d;
        d["style_id"]  = s->style_id;
        d["font_id"]   = s->font_id;
        d["font_size"] = s->font_size;
        d["color"]     = s->color;
        d["weight"]    = s->weight;
        d["italic"]    = s->italic;
        d["underline"] = s->underline;
        out.append(d);
    }
    return out;
}

static nb::list cursor_bboxes(bboxes_cursor* cur, bool include_formula = false) {
    nb::list out;
    while (auto* b = bboxes_next_bbox(cur)) {
        nb::dict d;
        d["page_id"]  = b->page_id;
        d["style_id"] = b->style_id;
        d["x"] = b->x;
        d["y"] = b->y;
        d["w"] = b->w;
        d["h"] = b->h;
        d["text"] = b->text;
        if (include_formula)
            d["formula"] = b->formula ? nb::cast(b->formula) : nb::none();
        out.append(d);
    }
    return out;
}

/* ── BBoxesCursor — wraps any backend cursor ─────────────────────── */

struct BBoxesCursor {
    bboxes_cursor* cur;
    std::vector<char> buf;

    /* PDF constructor */
    BBoxesCursor(nb::bytes data, std::optional<std::string> pw, int sp, int ep)
        : buf(data.c_str(), data.c_str() + data.size()) {
        cur = bboxes_open_pdf(buf.data(), buf.size(),
                               pw ? pw->c_str() : nullptr, sp, ep);
        if (!cur) throw nb::value_error("bad PDF");
    }

    nb::dict doc()      { return cursor_doc(cur); }
    nb::list pages()    { return cursor_pages(cur); }
    nb::list fonts()    { return cursor_fonts(cur); }
    nb::list styles()   { return cursor_styles(cur); }
    nb::list bboxes()   { return cursor_bboxes(cur); }
    void close() { if (cur) { bboxes_close(cur); cur = nullptr; } }
    ~BBoxesCursor() { close(); }
};

/* ── XLSX cursor ─────────────────────────────────────────────────── */

struct BBoxesXlsxCursor {
    bboxes_cursor* cur;
    std::vector<char> buf;

    BBoxesXlsxCursor(nb::bytes data, std::optional<std::string> pw, int sp, int ep)
        : buf(data.c_str(), data.c_str() + data.size()) {
        cur = bboxes_open_xlsx(buf.data(), buf.size(),
                                pw ? pw->c_str() : nullptr, sp, ep);
        if (!cur) throw nb::value_error("bad XLSX");
    }

    nb::dict doc()      { return cursor_doc(cur); }
    nb::list pages()    { return cursor_pages(cur); }
    nb::list fonts()    { return cursor_fonts(cur); }
    nb::list styles()   { return cursor_styles(cur); }
    nb::list bboxes()   { return cursor_bboxes(cur, true); }
    void close() { if (cur) { bboxes_close(cur); cur = nullptr; } }
    ~BBoxesXlsxCursor() { close(); }
};

/* ── Text cursor ─────────────────────────────────────────────────── */

struct BBoxesTextCursor {
    bboxes_cursor* cur;
    std::vector<char> buf;

    BBoxesTextCursor(nb::bytes data)
        : buf(data.c_str(), data.c_str() + data.size()) {
        cur = bboxes_open_text(buf.data(), buf.size());
        if (!cur) throw nb::value_error("bad text");
    }

    nb::dict doc()      { return cursor_doc(cur); }
    nb::list pages()    { return cursor_pages(cur); }
    nb::list fonts()    { return cursor_fonts(cur); }
    nb::list styles()   { return cursor_styles(cur); }
    nb::list bboxes()   { return cursor_bboxes(cur); }
    void close() { if (cur) { bboxes_close(cur); cur = nullptr; } }
    ~BBoxesTextCursor() { close(); }
};

/* ── DOCX cursor ─────────────────────────────────────────────────── */

struct BBoxesDocxCursor {
    bboxes_cursor* cur;
    std::vector<char> buf;

    BBoxesDocxCursor(nb::bytes data)
        : buf(data.c_str(), data.c_str() + data.size()) {
        cur = bboxes_open_docx(buf.data(), buf.size());
        if (!cur) throw nb::value_error("bad DOCX");
    }

    nb::dict doc()      { return cursor_doc(cur); }
    nb::list pages()    { return cursor_pages(cur); }
    nb::list fonts()    { return cursor_fonts(cur); }
    nb::list styles()   { return cursor_styles(cur); }
    nb::list bboxes()   { return cursor_bboxes(cur); }
    void close() { if (cur) { bboxes_close(cur); cur = nullptr; } }
    ~BBoxesDocxCursor() { close(); }
};

/* ── Auto-detecting cursor ───────────────────────────────────────── */

struct BBoxesAutoCursor {
    bboxes_cursor* cur;
    std::vector<char> buf;
    bool is_xlsx;

    BBoxesAutoCursor(nb::bytes data)
        : buf(data.c_str(), data.c_str() + data.size()) {
        const char* fmt = bboxes_detect(buf.data(), buf.size());
        is_xlsx = (fmt[0] == 'x');
        cur = bboxes_open(buf.data(), buf.size());
        if (!cur) throw nb::value_error("failed to parse document");
    }

    nb::dict doc()      { return cursor_doc(cur); }
    nb::list pages()    { return cursor_pages(cur); }
    nb::list fonts()    { return cursor_fonts(cur); }
    nb::list styles()   { return cursor_styles(cur); }
    nb::list bboxes()   { return cursor_bboxes(cur, is_xlsx); }
    void close() { if (cur) { bboxes_close(cur); cur = nullptr; } }
    ~BBoxesAutoCursor() { close(); }
};

/* ── detect + info convenience functions ─────────────────────────── */

static nb::str detect_format(nb::bytes data) {
    const char* fmt = bboxes_detect(data.c_str(), data.size());
    return nb::str(fmt);
}

static nb::dict info(nb::bytes data) {
    std::vector<char> buf(data.c_str(), data.c_str() + data.size());
    auto* cur = bboxes_open(buf.data(), buf.size());
    if (!cur) throw nb::value_error("failed to parse document");
    nb::dict out = cursor_doc(cur);
    bboxes_close(cur);
    return out;
}

/* ── JSON convenience functions ─────────────────────────────────────── */

/* helper: build JSON array from an iterator */
typedef bboxes_cursor* (*open_buf_fn)(const void*, size_t, const char*, int, int);

static nb::str json_array_pdf(nb::bytes data, std::optional<std::string> pw,
                               int sp, int ep,
                               const char* (*iter_fn)(bboxes_cursor*)) {
    std::vector<char> buf(data.c_str(), data.c_str() + data.size());
    auto* cur = bboxes_open_pdf(buf.data(), buf.size(),
                                 pw ? pw->c_str() : nullptr, sp, ep);
    if (!cur) throw nb::value_error("bad PDF");
    std::string result = "[";
    bool first = true;
    while (const char* json = iter_fn(cur)) {
        if (!first) result += ',';
        result += json;
        first = false;
    }
    bboxes_close(cur);
    result += ']';
    return nb::str(result.c_str(), result.size());
}

static nb::str doc_json(nb::bytes data, std::optional<std::string> pw, int sp, int ep) {
    std::vector<char> buf(data.c_str(), data.c_str() + data.size());
    auto* cur = bboxes_open_pdf(buf.data(), buf.size(),
                                 pw ? pw->c_str() : nullptr, sp, ep);
    if (!cur) throw nb::value_error("bad PDF");
    const char* json = bboxes_get_doc_json(cur);
    std::string result = json ? json : "null";
    bboxes_close(cur);
    return nb::str(result.c_str(), result.size());
}

static nb::str pages_json(nb::bytes data, std::optional<std::string> pw, int sp, int ep) {
    return json_array_pdf(data, pw, sp, ep, bboxes_next_page_json);
}

static nb::str fonts_json(nb::bytes data, std::optional<std::string> pw, int sp, int ep) {
    return json_array_pdf(data, pw, sp, ep, bboxes_next_font_json);
}

static nb::str styles_json(nb::bytes data, std::optional<std::string> pw, int sp, int ep) {
    return json_array_pdf(data, pw, sp, ep, bboxes_next_style_json);
}

static nb::str bboxes_json(nb::bytes data, std::optional<std::string> pw, int sp, int ep) {
    return json_array_pdf(data, pw, sp, ep, bboxes_next_bbox_json);
}

/* ── module definition ──────────────────────────────────────────────── */

NB_MODULE(blobboxes_ext, m) {
    m.def("_pdf_init", &bboxes_pdf_init);
    m.def("_pdf_destroy", &bboxes_pdf_destroy);
    m.def("_xlsx_init", &bboxes_xlsx_init);
    m.def("_xlsx_destroy", &bboxes_xlsx_destroy);

    /* PDF cursor (original) */
    nb::class_<BBoxesCursor>(m, "BBoxesCursor")
        .def(nb::init<nb::bytes, std::optional<std::string>, int, int>(),
             nb::arg("data"), nb::arg("password") = nb::none(),
             nb::arg("start_page") = 0, nb::arg("end_page") = 0)
        .def("doc", &BBoxesCursor::doc)
        .def("pages", &BBoxesCursor::pages)
        .def("fonts", &BBoxesCursor::fonts)
        .def("styles", &BBoxesCursor::styles)
        .def("bboxes", &BBoxesCursor::bboxes)
        .def("close", &BBoxesCursor::close);

    /* XLSX cursor */
    nb::class_<BBoxesXlsxCursor>(m, "BBoxesXlsxCursor")
        .def(nb::init<nb::bytes, std::optional<std::string>, int, int>(),
             nb::arg("data"), nb::arg("password") = nb::none(),
             nb::arg("start_page") = 0, nb::arg("end_page") = 0)
        .def("doc", &BBoxesXlsxCursor::doc)
        .def("pages", &BBoxesXlsxCursor::pages)
        .def("fonts", &BBoxesXlsxCursor::fonts)
        .def("styles", &BBoxesXlsxCursor::styles)
        .def("bboxes", &BBoxesXlsxCursor::bboxes)
        .def("close", &BBoxesXlsxCursor::close);

    /* Text cursor */
    nb::class_<BBoxesTextCursor>(m, "BBoxesTextCursor")
        .def(nb::init<nb::bytes>(), nb::arg("data"))
        .def("doc", &BBoxesTextCursor::doc)
        .def("pages", &BBoxesTextCursor::pages)
        .def("fonts", &BBoxesTextCursor::fonts)
        .def("styles", &BBoxesTextCursor::styles)
        .def("bboxes", &BBoxesTextCursor::bboxes)
        .def("close", &BBoxesTextCursor::close);

    /* DOCX cursor */
    nb::class_<BBoxesDocxCursor>(m, "BBoxesDocxCursor")
        .def(nb::init<nb::bytes>(), nb::arg("data"))
        .def("doc", &BBoxesDocxCursor::doc)
        .def("pages", &BBoxesDocxCursor::pages)
        .def("fonts", &BBoxesDocxCursor::fonts)
        .def("styles", &BBoxesDocxCursor::styles)
        .def("bboxes", &BBoxesDocxCursor::bboxes)
        .def("close", &BBoxesDocxCursor::close);

    /* Auto-detecting cursor */
    nb::class_<BBoxesAutoCursor>(m, "BBoxesAutoCursor")
        .def(nb::init<nb::bytes>(), nb::arg("data"))
        .def("doc", &BBoxesAutoCursor::doc)
        .def("pages", &BBoxesAutoCursor::pages)
        .def("fonts", &BBoxesAutoCursor::fonts)
        .def("styles", &BBoxesAutoCursor::styles)
        .def("bboxes", &BBoxesAutoCursor::bboxes)
        .def("close", &BBoxesAutoCursor::close);

    /* detect + info */
    m.def("detect", &detect_format, nb::arg("data"));
    m.def("info", &info, nb::arg("data"));

    /* PDF JSON functions (original) */
    m.def("doc_json", &doc_json,
          nb::arg("data"), nb::arg("password") = nb::none(),
          nb::arg("start_page") = 0, nb::arg("end_page") = 0);
    m.def("pages_json", &pages_json,
          nb::arg("data"), nb::arg("password") = nb::none(),
          nb::arg("start_page") = 0, nb::arg("end_page") = 0);
    m.def("fonts_json", &fonts_json,
          nb::arg("data"), nb::arg("password") = nb::none(),
          nb::arg("start_page") = 0, nb::arg("end_page") = 0);
    m.def("styles_json", &styles_json,
          nb::arg("data"), nb::arg("password") = nb::none(),
          nb::arg("start_page") = 0, nb::arg("end_page") = 0);
    m.def("bboxes_json", &bboxes_json,
          nb::arg("data"), nb::arg("password") = nb::none(),
          nb::arg("start_page") = 0, nb::arg("end_page") = 0);
}
