#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include "pdf_bboxes.h"
#include <vector>

namespace nb = nanobind;

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
    nb::str next() {
        auto* s = pdf_bboxes_extract_next(cur);
        if (!s) throw nb::stop_iteration();
        return nb::str(s);
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
    nb::str next() {
        auto* s = pdf_bboxes_fonts_next(cur);
        if (!s) throw nb::stop_iteration();
        return nb::str(s);
    }
    void close() { if (cur) { pdf_bboxes_fonts_close(cur); cur = nullptr; } }
    ~FontCursor() { close(); }
};

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
}
