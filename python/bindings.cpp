#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include "pdf_bboxes.h"
#include <string>
#include <vector>

namespace nb = nanobind;

/* Dict cursors — yield Python dicts */

struct ExtractCursor {
    pdf_bboxes_cursor* cur;
    std::vector<char> buf;

    ExtractCursor(nb::bytes data, std::optional<std::string> pw, int sp, int ep)
        : buf(data.c_str(), data.c_str() + data.size()) {
        cur = pdf_bboxes_extract_open(buf.data(), buf.size(),
                                       pw ? pw->c_str() : nullptr, sp, ep);
        if (!cur) throw nb::value_error("bad PDF");
    }
    ExtractCursor& iter() { return *this; }
    nb::dict next() {
        auto* r = pdf_bboxes_extract_next(cur);
        if (!r) throw nb::stop_iteration();
        nb::dict d;
        d["font_id"]   = r->font_id;
        d["page"]      = r->page;
        d["x"]         = r->x;
        d["y"]         = r->y;
        d["w"]         = r->w;
        d["h"]         = r->h;
        d["text"]      = r->text;
        d["color"]     = r->color;
        d["font_size"] = r->font_size;
        d["style"]     = r->style;
        return d;
    }
    void close() { if (cur) { pdf_bboxes_extract_close(cur); cur = nullptr; } }
    ~ExtractCursor() { close(); }
};

struct FontCursor {
    pdf_bboxes_font_cursor* cur;
    std::vector<char> buf;

    FontCursor(nb::bytes data, std::optional<std::string> pw)
        : buf(data.c_str(), data.c_str() + data.size()) {
        cur = pdf_bboxes_fonts_open(buf.data(), buf.size(),
                                     pw ? pw->c_str() : nullptr);
        if (!cur) throw nb::value_error("bad PDF");
    }
    FontCursor& iter() { return *this; }
    nb::dict next() {
        auto* f = pdf_bboxes_fonts_next(cur);
        if (!f) throw nb::stop_iteration();
        nb::dict d;
        d["font_id"] = f->font_id;
        d["name"]    = f->name;
        d["flags"]   = f->flags;
        d["style"]   = f->style;
        return d;
    }
    void close() { if (cur) { pdf_bboxes_fonts_close(cur); cur = nullptr; } }
    ~FontCursor() { close(); }
};

/* JSON functions — return complete JSON array strings */

static nb::str extract_json(nb::bytes data, std::optional<std::string> pw, int sp, int ep) {
    std::vector<char> buf(data.c_str(), data.c_str() + data.size());
    auto* cur = pdf_bboxes_extract_open(buf.data(), buf.size(),
                                         pw ? pw->c_str() : nullptr, sp, ep);
    if (!cur) throw nb::value_error("bad PDF");

    std::string result = "[";
    bool first = true;
    while (const char* json = pdf_bboxes_extract_next_json(cur)) {
        if (!first) result += ',';
        result += json;
        first = false;
    }
    pdf_bboxes_extract_close(cur);
    result += ']';
    return nb::str(result.c_str(), result.size());
}

static nb::str fonts_json(nb::bytes data, std::optional<std::string> pw) {
    std::vector<char> buf(data.c_str(), data.c_str() + data.size());
    auto* cur = pdf_bboxes_fonts_open(buf.data(), buf.size(),
                                       pw ? pw->c_str() : nullptr);
    if (!cur) throw nb::value_error("bad PDF");

    std::string result = "[";
    bool first = true;
    while (const char* json = pdf_bboxes_fonts_next_json(cur)) {
        if (!first) result += ',';
        result += json;
        first = false;
    }
    pdf_bboxes_fonts_close(cur);
    result += ']';
    return nb::str(result.c_str(), result.size());
}

NB_MODULE(pdf_bboxes_ext, m) {
    m.def("_init", &pdf_bboxes_init);
    m.def("_destroy", &pdf_bboxes_destroy);

    nb::class_<ExtractCursor>(m, "ExtractCursor")
        .def(nb::init<nb::bytes, std::optional<std::string>, int, int>(),
             nb::arg("data"), nb::arg("password") = nb::none(),
             nb::arg("start_page") = 0, nb::arg("end_page") = 0)
        .def("__iter__", &ExtractCursor::iter, nb::rv_policy::reference)
        .def("__next__", &ExtractCursor::next)
        .def("close", &ExtractCursor::close);

    nb::class_<FontCursor>(m, "FontCursor")
        .def(nb::init<nb::bytes, std::optional<std::string>>(),
             nb::arg("data"), nb::arg("password") = nb::none())
        .def("__iter__", &FontCursor::iter, nb::rv_policy::reference)
        .def("__next__", &FontCursor::next)
        .def("close", &FontCursor::close);

    m.def("extract_json", &extract_json,
          nb::arg("data"), nb::arg("password") = nb::none(),
          nb::arg("start_page") = 0, nb::arg("end_page") = 0);
    m.def("fonts_json", &fonts_json,
          nb::arg("data"), nb::arg("password") = nb::none());
}
