#define DUCKDB_EXTENSION_NAME bboxes
#include "duckdb.h"
#include "duckdb_extension.h"

DUCKDB_EXTENSION_EXTERN

#include "bboxes.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* ── helpers ──────────────────────────────────────────────────────── */

static std::vector<char> read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    std::vector<char> buf(sz);
    if (fread(buf.data(), 1, sz, f) != static_cast<size_t>(sz))
        buf.clear();
    fclose(f);
    return buf;
}

static const char* get_string(duckdb_vector vec, idx_t i) {
    auto* s = &static_cast<duckdb_string_t*>(duckdb_vector_get_data(vec))[i];
    return s->value.inlined.length > 12 ? s->value.pointer.ptr : s->value.inlined.inlined;
}

/* ── Format enum and dispatch ────────────────────────────────────── */

enum class Format { AUTO, PDF, XLSX, TEXT, DOCX };

static bboxes_cursor* open_by_format(Format fmt, const void* buf, size_t len) {
    switch (fmt) {
        case Format::AUTO:  return bboxes_open(buf, len);
        case Format::PDF:   return bboxes_open_pdf(buf, len, nullptr, 0, 0);
        case Format::XLSX:  return bboxes_open_xlsx(buf, len, nullptr, 0, 0);
        case Format::TEXT:  return bboxes_open_text(buf, len);
        case Format::DOCX:  return bboxes_open_docx(buf, len);
    }
    return nullptr;
}

static const char* format_name(Format fmt) {
    switch (fmt) {
        case Format::AUTO:  return "file";
        case Format::PDF:   return "PDF";
        case Format::XLSX:  return "XLSX";
        case Format::TEXT:  return "text";
        case Format::DOCX:  return "DOCX";
    }
    return "unknown";
}

/* ── shared bind/init ────────────────────────────────────────────── */

struct BindData { char* file_path; };

struct InitData {
    std::vector<char> buf;
    bboxes_cursor* cursor;
    Format fmt;
};

static void shared_bind_path(duckdb_bind_info info, BindData** out) {
    duckdb_value val = duckdb_bind_get_parameter(info, 0);
    char* path = duckdb_get_varchar(val);
    duckdb_destroy_value(&val);
    auto* data = new BindData{path};
    duckdb_bind_set_bind_data(info, data, [](void* p) {
        auto* d = static_cast<BindData*>(p);
        duckdb_free(d->file_path);
        delete d;
    });
    *out = data;
}

/* Generic init — reads Format from extra_info (table functions)
   or defaults to AUTO when extra_info is null. */
static void generic_init(duckdb_init_info info) {
    auto* bind = static_cast<BindData*>(duckdb_init_get_bind_data(info));
    auto* fmt_ptr = static_cast<Format*>(duckdb_init_get_extra_info(info));
    Format fmt = fmt_ptr ? *fmt_ptr : Format::AUTO;

    auto* data = new InitData{};
    data->fmt = fmt;
    data->buf = read_file(bind->file_path);
    if (data->buf.empty()) {
        duckdb_init_set_error(info, "failed to read file");
        delete data;
        return;
    }
    data->cursor = open_by_format(fmt, data->buf.data(), data->buf.size());
    if (!data->cursor) {
        std::string msg = std::string("failed to parse ") + format_name(fmt);
        duckdb_init_set_error(info, msg.c_str());
        delete data;
        return;
    }
    duckdb_init_set_max_threads(info, 1);
    duckdb_init_set_init_data(info, data, [](void* p) {
        auto* d = static_cast<InitData*>(p);
        if (d->cursor) bboxes_close(d->cursor);
        delete d;
    });
}

/* ── table function: doc ─────────────────────────────────────────── */

static void doc_bind(duckdb_bind_info info) {
    BindData* data;
    shared_bind_path(info, &data);

    duckdb_logical_type t_int = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
    duckdb_logical_type t_str = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);

    duckdb_bind_add_result_column(info, "document_id", t_int);
    duckdb_bind_add_result_column(info, "source_type", t_str);
    duckdb_bind_add_result_column(info, "filename", t_str);
    duckdb_bind_add_result_column(info, "checksum", t_str);
    duckdb_bind_add_result_column(info, "page_count", t_int);

    duckdb_destroy_logical_type(&t_int);
    duckdb_destroy_logical_type(&t_str);
}

static void doc_func(duckdb_function_info info, duckdb_data_chunk output) {
    auto* data = static_cast<InitData*>(duckdb_function_get_init_data(info));
    const bboxes_doc* d = bboxes_get_doc(data->cursor);
    if (!d) { duckdb_data_chunk_set_size(output, 0); return; }

    auto* doc_id_data = static_cast<int32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 0)));
    auto* page_count_data = static_cast<int32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 4)));

    doc_id_data[0] = static_cast<int32_t>(d->document_id);
    duckdb_vector_assign_string_element(duckdb_data_chunk_get_vector(output, 1), 0, d->source_type);
    duckdb_vector v_filename = duckdb_data_chunk_get_vector(output, 2);
    if (d->filename)
        duckdb_vector_assign_string_element(v_filename, 0, d->filename);
    else {
        uint64_t* validity = duckdb_vector_get_validity(v_filename);
        if (validity) validity[0] &= ~(uint64_t(1));
        else {
            duckdb_vector_ensure_validity_writable(v_filename);
            validity = duckdb_vector_get_validity(v_filename);
            validity[0] &= ~(uint64_t(1));
        }
    }
    duckdb_vector_assign_string_element(duckdb_data_chunk_get_vector(output, 3), 0, d->checksum);
    page_count_data[0] = d->page_count;

    duckdb_data_chunk_set_size(output, 1);
    bboxes_close(data->cursor);
    data->cursor = nullptr;
}

/* ── table function: pages ───────────────────────────────────────── */

static void pages_bind(duckdb_bind_info info) {
    BindData* data;
    shared_bind_path(info, &data);

    duckdb_logical_type t_int = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
    duckdb_logical_type t_dbl = duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE);

    duckdb_bind_add_result_column(info, "page_id", t_int);
    duckdb_bind_add_result_column(info, "document_id", t_int);
    duckdb_bind_add_result_column(info, "page_number", t_int);
    duckdb_bind_add_result_column(info, "width", t_dbl);
    duckdb_bind_add_result_column(info, "height", t_dbl);

    duckdb_destroy_logical_type(&t_int);
    duckdb_destroy_logical_type(&t_dbl);
}

static void pages_func(duckdb_function_info info, duckdb_data_chunk output) {
    auto* data = static_cast<InitData*>(duckdb_function_get_init_data(info));
    if (!data->cursor) { duckdb_data_chunk_set_size(output, 0); return; }

    auto* page_id_data  = static_cast<int32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 0)));
    auto* doc_id_data   = static_cast<int32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 1)));
    auto* pagenum_data  = static_cast<int32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 2)));
    auto* width_data    = static_cast<double*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 3)));
    auto* height_data   = static_cast<double*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 4)));

    idx_t row = 0;
    while (row < 2048) {
        const bboxes_page* p = bboxes_next_page(data->cursor);
        if (!p) break;
        page_id_data[row] = static_cast<int32_t>(p->page_id);
        doc_id_data[row]  = static_cast<int32_t>(p->document_id);
        pagenum_data[row] = p->page_number;
        width_data[row]   = p->width;
        height_data[row]  = p->height;
        row++;
    }
    duckdb_data_chunk_set_size(output, row);
}

/* ── table function: fonts ───────────────────────────────────────── */

static void fonts_bind(duckdb_bind_info info) {
    BindData* data;
    shared_bind_path(info, &data);

    duckdb_logical_type t_int = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
    duckdb_logical_type t_str = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);

    duckdb_bind_add_result_column(info, "font_id", t_int);
    duckdb_bind_add_result_column(info, "name", t_str);

    duckdb_destroy_logical_type(&t_int);
    duckdb_destroy_logical_type(&t_str);
}

static void fonts_func(duckdb_function_info info, duckdb_data_chunk output) {
    auto* data = static_cast<InitData*>(duckdb_function_get_init_data(info));
    if (!data->cursor) { duckdb_data_chunk_set_size(output, 0); return; }

    auto* font_id_data = static_cast<int32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 0)));
    duckdb_vector v_name = duckdb_data_chunk_get_vector(output, 1);

    idx_t row = 0;
    while (row < 2048) {
        const bboxes_font* f = bboxes_next_font(data->cursor);
        if (!f) break;
        font_id_data[row] = static_cast<int32_t>(f->font_id);
        duckdb_vector_assign_string_element(v_name, row, f->name);
        row++;
    }
    duckdb_data_chunk_set_size(output, row);
}

/* ── table function: styles ──────────────────────────────────────── */

static void styles_bind(duckdb_bind_info info) {
    BindData* data;
    shared_bind_path(info, &data);

    duckdb_logical_type t_int = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
    duckdb_logical_type t_dbl = duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE);
    duckdb_logical_type t_str = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);

    duckdb_bind_add_result_column(info, "style_id", t_int);
    duckdb_bind_add_result_column(info, "font_id", t_int);
    duckdb_bind_add_result_column(info, "font_size", t_dbl);
    duckdb_bind_add_result_column(info, "color", t_str);
    duckdb_bind_add_result_column(info, "weight", t_str);
    duckdb_bind_add_result_column(info, "italic", t_int);
    duckdb_bind_add_result_column(info, "underline", t_int);

    duckdb_destroy_logical_type(&t_int);
    duckdb_destroy_logical_type(&t_dbl);
    duckdb_destroy_logical_type(&t_str);
}

static void styles_func(duckdb_function_info info, duckdb_data_chunk output) {
    auto* data = static_cast<InitData*>(duckdb_function_get_init_data(info));
    if (!data->cursor) { duckdb_data_chunk_set_size(output, 0); return; }

    auto* style_id_data  = static_cast<int32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 0)));
    auto* font_id_data   = static_cast<int32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 1)));
    auto* font_size_data = static_cast<double*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 2)));
    duckdb_vector v_color  = duckdb_data_chunk_get_vector(output, 3);
    duckdb_vector v_weight = duckdb_data_chunk_get_vector(output, 4);
    auto* italic_data    = static_cast<int32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 5)));
    auto* underline_data = static_cast<int32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 6)));

    idx_t row = 0;
    while (row < 2048) {
        const bboxes_style* s = bboxes_next_style(data->cursor);
        if (!s) break;
        style_id_data[row]  = static_cast<int32_t>(s->style_id);
        font_id_data[row]   = static_cast<int32_t>(s->font_id);
        font_size_data[row] = s->font_size;
        duckdb_vector_assign_string_element(v_color, row, s->color);
        duckdb_vector_assign_string_element(v_weight, row, s->weight);
        italic_data[row]    = s->italic;
        underline_data[row] = s->underline;
        row++;
    }
    duckdb_data_chunk_set_size(output, row);
}

/* ── table function: bboxes ──────────────────────────────────────── */

static void bboxes_bind(duckdb_bind_info info) {
    BindData* data;
    shared_bind_path(info, &data);

    duckdb_logical_type t_int = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
    duckdb_logical_type t_dbl = duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE);
    duckdb_logical_type t_str = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);

    duckdb_bind_add_result_column(info, "page_id", t_int);
    duckdb_bind_add_result_column(info, "style_id", t_int);
    duckdb_bind_add_result_column(info, "x", t_dbl);
    duckdb_bind_add_result_column(info, "y", t_dbl);
    duckdb_bind_add_result_column(info, "w", t_dbl);
    duckdb_bind_add_result_column(info, "h", t_dbl);
    duckdb_bind_add_result_column(info, "text", t_str);
    duckdb_bind_add_result_column(info, "formula", t_str);

    duckdb_destroy_logical_type(&t_int);
    duckdb_destroy_logical_type(&t_dbl);
    duckdb_destroy_logical_type(&t_str);
}

static void bboxes_func(duckdb_function_info info, duckdb_data_chunk output) {
    auto* data = static_cast<InitData*>(duckdb_function_get_init_data(info));
    if (!data->cursor) { duckdb_data_chunk_set_size(output, 0); return; }

    auto* page_id_data  = static_cast<int32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 0)));
    auto* style_id_data = static_cast<int32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 1)));
    auto* x_data = static_cast<double*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 2)));
    auto* y_data = static_cast<double*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 3)));
    auto* w_data = static_cast<double*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 4)));
    auto* h_data = static_cast<double*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 5)));
    duckdb_vector v_text = duckdb_data_chunk_get_vector(output, 6);
    duckdb_vector v_formula = duckdb_data_chunk_get_vector(output, 7);

    idx_t row = 0;
    while (row < 2048) {
        const bboxes_bbox* b = bboxes_next_bbox(data->cursor);
        if (!b) break;
        page_id_data[row]  = static_cast<int32_t>(b->page_id);
        style_id_data[row] = static_cast<int32_t>(b->style_id);
        x_data[row] = b->x;
        y_data[row] = b->y;
        w_data[row] = b->w;
        h_data[row] = b->h;
        duckdb_vector_assign_string_element(v_text, row, b->text);
        if (b->formula) {
            duckdb_vector_assign_string_element(v_formula, row, b->formula);
        } else {
            duckdb_vector_ensure_validity_writable(v_formula);
            uint64_t* validity = duckdb_vector_get_validity(v_formula);
            validity[row / 64] &= ~(uint64_t(1) << (row % 64));
        }
        row++;
    }
    duckdb_data_chunk_set_size(output, row);
}

/* ── generic JSON scalar dispatch ────────────────────────────────── */

typedef const char* (*json_iter_fn)(bboxes_cursor*);

struct ScalarDesc {
    Format fmt;
    json_iter_fn iter_fn;
    bool is_single;   /* true = bboxes_get_doc_json (single object), false = array */
};

static void generic_json_scalar(duckdb_function_info info, duckdb_data_chunk input,
                                 duckdb_vector output) {
    auto* desc = static_cast<ScalarDesc*>(duckdb_scalar_function_get_extra_info(info));
    idx_t count = duckdb_data_chunk_get_size(input);
    duckdb_vector v_path = duckdb_data_chunk_get_vector(input, 0);

    for (idx_t i = 0; i < count; i++) {
        const char* path = get_string(v_path, i);
        auto buf = read_file(path);

        if (desc->is_single) {
            std::string result = "null";
            if (!buf.empty()) {
                auto* cur = open_by_format(desc->fmt, buf.data(), buf.size());
                if (cur) {
                    const char* json = desc->iter_fn(cur);
                    if (json) result = json;
                    bboxes_close(cur);
                }
            }
            duckdb_vector_assign_string_element_len(output, i, result.c_str(), result.size());
        } else {
            std::string result = "[";
            if (!buf.empty()) {
                auto* cur = open_by_format(desc->fmt, buf.data(), buf.size());
                if (cur) {
                    bool first = true;
                    while (const char* json = desc->iter_fn(cur)) {
                        if (!first) result += ',';
                        result += json;
                        first = false;
                    }
                    bboxes_close(cur);
                }
            }
            result += ']';
            duckdb_vector_assign_string_element_len(output, i, result.c_str(), result.size());
        }
    }
}

/* ── registration helpers ────────────────────────────────────────── */

static void register_table_fn(duckdb_connection conn, const char* name,
                               duckdb_table_function_bind_t bind_fn,
                               duckdb_table_function_t func_fn,
                               Format* fmt_ptr) {
    duckdb_table_function func = duckdb_create_table_function();
    duckdb_table_function_set_name(func, name);
    duckdb_logical_type t = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    duckdb_table_function_add_parameter(func, t);
    duckdb_destroy_logical_type(&t);
    duckdb_table_function_set_bind(func, bind_fn);
    duckdb_table_function_set_init(func, generic_init);
    duckdb_table_function_set_function(func, func_fn);
    if (fmt_ptr)
        duckdb_table_function_set_extra_info(func, fmt_ptr, nullptr);
    duckdb_register_table_function(conn, func);
    duckdb_destroy_table_function(&func);
}

static void register_json_scalar(duckdb_connection conn, const char* name,
                                  ScalarDesc* desc, bool varargs) {
    duckdb_scalar_function func = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(func, name);

    duckdb_logical_type t_str = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    duckdb_scalar_function_add_parameter(func, t_str);
    if (varargs) {
        duckdb_logical_type t_big = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
        duckdb_scalar_function_set_varargs(func, t_big);
        duckdb_destroy_logical_type(&t_big);
    }
    duckdb_scalar_function_set_return_type(func, t_str);
    duckdb_destroy_logical_type(&t_str);

    duckdb_scalar_function_set_extra_info(func, desc, nullptr);
    duckdb_scalar_function_set_function(func, generic_json_scalar);
    duckdb_register_scalar_function(conn, func);
    duckdb_destroy_scalar_function(&func);
}

/* ── format descriptor table ─────────────────────────────────────── */

struct FormatInfo {
    const char* prefix;      /* e.g. "bboxes_xlsx" or "bboxes" for auto */
    Format      fmt;
    bool        varargs;     /* auto-detect aliases accept varargs */
};

static Format s_fmts[] = {
    Format::AUTO, Format::PDF, Format::XLSX, Format::TEXT, Format::DOCX
};

static ScalarDesc s_scalars[][5] = {
    /* AUTO */
    { {Format::AUTO, bboxes_get_doc_json,   true},
      {Format::AUTO, bboxes_next_page_json, false},
      {Format::AUTO, bboxes_next_font_json, false},
      {Format::AUTO, bboxes_next_style_json,false},
      {Format::AUTO, bboxes_next_bbox_json, false} },
    /* PDF */
    { {Format::PDF, bboxes_get_doc_json,   true},
      {Format::PDF, bboxes_next_page_json, false},
      {Format::PDF, bboxes_next_font_json, false},
      {Format::PDF, bboxes_next_style_json,false},
      {Format::PDF, bboxes_next_bbox_json, false} },
    /* XLSX */
    { {Format::XLSX, bboxes_get_doc_json,   true},
      {Format::XLSX, bboxes_next_page_json, false},
      {Format::XLSX, bboxes_next_font_json, false},
      {Format::XLSX, bboxes_next_style_json,false},
      {Format::XLSX, bboxes_next_bbox_json, false} },
    /* TEXT */
    { {Format::TEXT, bboxes_get_doc_json,   true},
      {Format::TEXT, bboxes_next_page_json, false},
      {Format::TEXT, bboxes_next_font_json, false},
      {Format::TEXT, bboxes_next_style_json,false},
      {Format::TEXT, bboxes_next_bbox_json, false} },
    /* DOCX */
    { {Format::DOCX, bboxes_get_doc_json,   true},
      {Format::DOCX, bboxes_next_page_json, false},
      {Format::DOCX, bboxes_next_font_json, false},
      {Format::DOCX, bboxes_next_style_json,false},
      {Format::DOCX, bboxes_next_bbox_json, false} },
};

static const FormatInfo s_formats[] = {
    { "bboxes",      Format::AUTO, true  },
    { "bboxes_pdf",  Format::PDF,  false },   /* PDF uses same names as auto for table fns */
    { "bboxes_xlsx", Format::XLSX, false },
    { "bboxes_text", Format::TEXT, false },
    { "bboxes_docx", Format::DOCX, false },
};

static const char* s_table_suffixes[] = { "_doc", "_pages", "_fonts", "_styles", "" };
static duckdb_table_function_bind_t s_bind_fns[] = { doc_bind, pages_bind, fonts_bind, styles_bind, bboxes_bind };
static duckdb_table_function_t s_func_fns[] = { doc_func, pages_func, fonts_func, styles_func, bboxes_func };
static const char* s_json_suffixes[] = { "_doc_json", "_pages_json", "_fonts_json", "_styles_json", "_json" };

/* ── entrypoint ──────────────────────────────────────────────────── */

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_connection connection, duckdb_extension_info info,
                            struct duckdb_extension_access* access) {
    bboxes_pdf_init();
    bboxes_xlsx_init();

    for (int fi = 0; fi < 5; fi++) {
        const FormatInfo& f = s_formats[fi];
        Format* fmt_ptr = &s_fmts[fi];

        for (int ti = 0; ti < 5; ti++) {
            std::string name = std::string(f.prefix) + s_table_suffixes[ti];
            register_table_fn(connection, name.c_str(), s_bind_fns[ti], s_func_fns[ti], fmt_ptr);
        }

        for (int si = 0; si < 5; si++) {
            std::string name = std::string(f.prefix) + s_json_suffixes[si];
            register_json_scalar(connection, name.c_str(), &s_scalars[fi][si], f.varargs);
        }
    }

    /* bboxes_info — auto-detecting doc info scalar (alias of bboxes_doc_json) */
    register_json_scalar(connection, "bboxes_info", &s_scalars[0][0], false);

    return true;
}
