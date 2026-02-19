#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include "bboxes.h"
#include <string>
#include <vector>

namespace nb = nanobind;

/* ── BBoxesCursor — single cursor with iterators for each object type ── */

struct BBoxesCursor {
    bboxes_cursor* cur;
    std::vector<char> buf;

    BBoxesCursor(nb::bytes data, std::optional<std::string> pw, int sp, int ep)
        : buf(data.c_str(), data.c_str() + data.size()) {
        cur = bboxes_open_pdf(buf.data(), buf.size(),
                               pw ? pw->c_str() : nullptr, sp, ep);
        if (!cur) throw nb::value_error("bad PDF");
    }

    nb::dict doc() {
        auto* d = bboxes_get_doc(cur);
        nb::dict out;
        out["document_id"] = d->document_id;
        out["source_type"] = d->source_type;
        out["filename"]    = d->filename ? nb::cast(d->filename) : nb::none();
        out["page_count"]  = d->page_count;
        return out;
    }

    nb::list pages() {
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

    nb::list fonts() {
        nb::list out;
        while (auto* f = bboxes_next_font(cur)) {
            nb::dict d;
            d["font_id"] = f->font_id;
            d["name"]    = f->name;
            out.append(d);
        }
        return out;
    }

    nb::list styles() {
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

    nb::list bboxes() {
        nb::list out;
        while (auto* b = bboxes_next_bbox(cur)) {
            nb::dict d;
            d["bbox_id"]  = b->bbox_id;
            d["page_id"]  = b->page_id;
            d["style_id"] = b->style_id;
            d["x"] = b->x;
            d["y"] = b->y;
            d["w"] = b->w;
            d["h"] = b->h;
            d["text"] = b->text;
            out.append(d);
        }
        return out;
    }

    void close() { if (cur) { bboxes_close(cur); cur = nullptr; } }
    ~BBoxesCursor() { close(); }
};

/* ── JSON convenience functions ─────────────────────────────────────── */

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
    std::vector<char> buf(data.c_str(), data.c_str() + data.size());
    auto* cur = bboxes_open_pdf(buf.data(), buf.size(),
                                 pw ? pw->c_str() : nullptr, sp, ep);
    if (!cur) throw nb::value_error("bad PDF");
    std::string result = "[";
    bool first = true;
    while (const char* json = bboxes_next_page_json(cur)) {
        if (!first) result += ',';
        result += json;
        first = false;
    }
    bboxes_close(cur);
    result += ']';
    return nb::str(result.c_str(), result.size());
}

static nb::str fonts_json(nb::bytes data, std::optional<std::string> pw, int sp, int ep) {
    std::vector<char> buf(data.c_str(), data.c_str() + data.size());
    auto* cur = bboxes_open_pdf(buf.data(), buf.size(),
                                 pw ? pw->c_str() : nullptr, sp, ep);
    if (!cur) throw nb::value_error("bad PDF");
    std::string result = "[";
    bool first = true;
    while (const char* json = bboxes_next_font_json(cur)) {
        if (!first) result += ',';
        result += json;
        first = false;
    }
    bboxes_close(cur);
    result += ']';
    return nb::str(result.c_str(), result.size());
}

static nb::str styles_json(nb::bytes data, std::optional<std::string> pw, int sp, int ep) {
    std::vector<char> buf(data.c_str(), data.c_str() + data.size());
    auto* cur = bboxes_open_pdf(buf.data(), buf.size(),
                                 pw ? pw->c_str() : nullptr, sp, ep);
    if (!cur) throw nb::value_error("bad PDF");
    std::string result = "[";
    bool first = true;
    while (const char* json = bboxes_next_style_json(cur)) {
        if (!first) result += ',';
        result += json;
        first = false;
    }
    bboxes_close(cur);
    result += ']';
    return nb::str(result.c_str(), result.size());
}

static nb::str bboxes_json(nb::bytes data, std::optional<std::string> pw, int sp, int ep) {
    std::vector<char> buf(data.c_str(), data.c_str() + data.size());
    auto* cur = bboxes_open_pdf(buf.data(), buf.size(),
                                 pw ? pw->c_str() : nullptr, sp, ep);
    if (!cur) throw nb::value_error("bad PDF");
    std::string result = "[";
    bool first = true;
    while (const char* json = bboxes_next_bbox_json(cur)) {
        if (!first) result += ',';
        result += json;
        first = false;
    }
    bboxes_close(cur);
    result += ']';
    return nb::str(result.c_str(), result.size());
}

/* ── module definition ──────────────────────────────────────────────── */

NB_MODULE(bboxes_ext, m) {
    m.def("_pdf_init", &bboxes_pdf_init);
    m.def("_pdf_destroy", &bboxes_pdf_destroy);

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
