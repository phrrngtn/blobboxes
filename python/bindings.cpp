#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/optional.h>

#include "pdf_bboxes.h"

#include <string>
#include <vector>

namespace nb = nanobind;

static int collect_callback(const char* json, void* user_data) {
    auto* results = static_cast<std::vector<std::string>*>(user_data);
    results->push_back(json);
    return 0;
}

static nb::object call_and_parse(
    int (*api_fn)(const void*, size_t, const char*, pdf_bbox_callback, void*),
    nb::bytes data,
    std::optional<std::string> password
) {
    std::vector<std::string> json_strings;

    const char* pw = password ? password->c_str() : nullptr;

    int rc = api_fn(data.c_str(), data.size(), pw, collect_callback, &json_strings);
    if (rc == -1) {
        throw nb::value_error("Failed to process PDF (invalid file or password)");
    }

    nb::module_ json_mod = nb::module_::import_("json");
    nb::object loads = json_mod.attr("loads");

    nb::list result;
    for (const auto& s : json_strings) {
        result.append(loads(nb::str(s.c_str(), s.size())));
    }
    return result;
}

NB_MODULE(pdf_bboxes_ext, m) {
    m.def("_init", &pdf_bboxes_init, "Initialize the PDF library");
    m.def("_destroy", &pdf_bboxes_destroy, "Destroy the PDF library");

    m.def("_extract", [](nb::bytes data, std::optional<std::string> password) {
        return call_and_parse(pdf_bboxes_extract, data, password);
    }, nb::arg("data"), nb::arg("password") = nb::none(),
       "Extract text bounding boxes from PDF bytes");

    m.def("_fonts", [](nb::bytes data, std::optional<std::string> password) {
        return call_and_parse(pdf_bboxes_fonts, data, password);
    }, nb::arg("data"), nb::arg("password") = nb::none(),
       "Extract font table from PDF bytes");
}
