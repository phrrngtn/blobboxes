#define DUCKDB_EXTENSION_NAME pdf_bboxes
#include "duckdb.h"
#include "duckdb_extension.h"

DUCKDB_EXTENSION_EXTERN

#include "pdf_bboxes.h"
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

/* ── pdf_extract table function ──────────────────────────────────── */

struct ExtractBindData {
    char* file_path;
};

struct ExtractInitData {
    std::vector<char> buf;
    pdf_bboxes_cursor* cursor;
};

static void extract_bind(duckdb_bind_info info) {
    duckdb_value val = duckdb_bind_get_parameter(info, 0);
    char* path = duckdb_get_varchar(val);
    duckdb_destroy_value(&val);

    auto* data = new ExtractBindData{path};
    duckdb_bind_set_bind_data(info, data, [](void* p) {
        auto* d = static_cast<ExtractBindData*>(p);
        duckdb_free(d->file_path);
        delete d;
    });

    duckdb_logical_type t_int = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
    duckdb_logical_type t_dbl = duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE);
    duckdb_logical_type t_str = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);

    duckdb_bind_add_result_column(info, "font_id", t_int);
    duckdb_bind_add_result_column(info, "page", t_int);
    duckdb_bind_add_result_column(info, "x", t_dbl);
    duckdb_bind_add_result_column(info, "y", t_dbl);
    duckdb_bind_add_result_column(info, "w", t_dbl);
    duckdb_bind_add_result_column(info, "h", t_dbl);
    duckdb_bind_add_result_column(info, "text", t_str);
    duckdb_bind_add_result_column(info, "color", t_str);
    duckdb_bind_add_result_column(info, "font_size", t_dbl);
    duckdb_bind_add_result_column(info, "style", t_str);

    duckdb_destroy_logical_type(&t_int);
    duckdb_destroy_logical_type(&t_dbl);
    duckdb_destroy_logical_type(&t_str);
}

static void extract_init(duckdb_init_info info) {
    auto* bind = static_cast<ExtractBindData*>(duckdb_init_get_bind_data(info));
    auto* data = new ExtractInitData{};
    data->buf = read_file(bind->file_path);
    if (data->buf.empty()) {
        duckdb_init_set_error(info, "failed to read PDF file");
        delete data;
        return;
    }
    data->cursor = pdf_bboxes_extract_open(data->buf.data(), data->buf.size(), nullptr, 0, 0);
    if (!data->cursor) {
        duckdb_init_set_error(info, "failed to parse PDF");
        delete data;
        return;
    }
    duckdb_init_set_max_threads(info, 1);
    duckdb_init_set_init_data(info, data, [](void* p) {
        auto* d = static_cast<ExtractInitData*>(p);
        if (d->cursor) pdf_bboxes_extract_close(d->cursor);
        delete d;
    });
}

static void extract_func(duckdb_function_info info, duckdb_data_chunk output) {
    auto* data = static_cast<ExtractInitData*>(duckdb_function_get_init_data(info));

    duckdb_vector v_font_id   = duckdb_data_chunk_get_vector(output, 0);
    duckdb_vector v_page      = duckdb_data_chunk_get_vector(output, 1);
    duckdb_vector v_x         = duckdb_data_chunk_get_vector(output, 2);
    duckdb_vector v_y         = duckdb_data_chunk_get_vector(output, 3);
    duckdb_vector v_w         = duckdb_data_chunk_get_vector(output, 4);
    duckdb_vector v_h         = duckdb_data_chunk_get_vector(output, 5);
    duckdb_vector v_text      = duckdb_data_chunk_get_vector(output, 6);
    duckdb_vector v_color     = duckdb_data_chunk_get_vector(output, 7);
    duckdb_vector v_font_size = duckdb_data_chunk_get_vector(output, 8);
    duckdb_vector v_style     = duckdb_data_chunk_get_vector(output, 9);

    auto* font_id_data   = static_cast<int32_t*>(duckdb_vector_get_data(v_font_id));
    auto* page_data      = static_cast<int32_t*>(duckdb_vector_get_data(v_page));
    auto* x_data         = static_cast<double*>(duckdb_vector_get_data(v_x));
    auto* y_data         = static_cast<double*>(duckdb_vector_get_data(v_y));
    auto* w_data         = static_cast<double*>(duckdb_vector_get_data(v_w));
    auto* h_data         = static_cast<double*>(duckdb_vector_get_data(v_h));
    auto* font_size_data = static_cast<double*>(duckdb_vector_get_data(v_font_size));

    idx_t row = 0;
    idx_t max_rows = 2048;
    while (row < max_rows) {
        const pdf_bboxes_run* r = pdf_bboxes_extract_next(data->cursor);
        if (!r) break;

        font_id_data[row]   = static_cast<int32_t>(r->font_id);
        page_data[row]      = r->page;
        x_data[row]         = r->x;
        y_data[row]         = r->y;
        w_data[row]         = r->w;
        h_data[row]         = r->h;
        font_size_data[row] = r->font_size;
        duckdb_vector_assign_string_element(v_text,  row, r->text);
        duckdb_vector_assign_string_element(v_color, row, r->color);
        duckdb_vector_assign_string_element(v_style, row, r->style);
        row++;
    }
    duckdb_data_chunk_set_size(output, row);
}

/* ── pdf_fonts table function ────────────────────────────────────── */

struct FontsBindData {
    char* file_path;
};

struct FontsInitData {
    std::vector<char> buf;
    pdf_bboxes_font_cursor* cursor;
};

static void fonts_bind(duckdb_bind_info info) {
    duckdb_value val = duckdb_bind_get_parameter(info, 0);
    char* path = duckdb_get_varchar(val);
    duckdb_destroy_value(&val);

    auto* data = new FontsBindData{path};
    duckdb_bind_set_bind_data(info, data, [](void* p) {
        auto* d = static_cast<FontsBindData*>(p);
        duckdb_free(d->file_path);
        delete d;
    });

    duckdb_logical_type t_int = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
    duckdb_logical_type t_str = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);

    duckdb_bind_add_result_column(info, "font_id", t_int);
    duckdb_bind_add_result_column(info, "name", t_str);
    duckdb_bind_add_result_column(info, "flags", t_int);
    duckdb_bind_add_result_column(info, "style", t_str);

    duckdb_destroy_logical_type(&t_int);
    duckdb_destroy_logical_type(&t_str);
}

static void fonts_init(duckdb_init_info info) {
    auto* bind = static_cast<FontsBindData*>(duckdb_init_get_bind_data(info));
    auto* data = new FontsInitData{};
    data->buf = read_file(bind->file_path);
    if (data->buf.empty()) {
        duckdb_init_set_error(info, "failed to read PDF file");
        delete data;
        return;
    }
    data->cursor = pdf_bboxes_fonts_open(data->buf.data(), data->buf.size(), nullptr);
    if (!data->cursor) {
        duckdb_init_set_error(info, "failed to parse PDF");
        delete data;
        return;
    }
    duckdb_init_set_max_threads(info, 1);
    duckdb_init_set_init_data(info, data, [](void* p) {
        auto* d = static_cast<FontsInitData*>(p);
        if (d->cursor) pdf_bboxes_fonts_close(d->cursor);
        delete d;
    });
}

static void fonts_func(duckdb_function_info info, duckdb_data_chunk output) {
    auto* data = static_cast<FontsInitData*>(duckdb_function_get_init_data(info));

    duckdb_vector v_font_id = duckdb_data_chunk_get_vector(output, 0);
    duckdb_vector v_name    = duckdb_data_chunk_get_vector(output, 1);
    duckdb_vector v_flags   = duckdb_data_chunk_get_vector(output, 2);
    duckdb_vector v_style   = duckdb_data_chunk_get_vector(output, 3);

    auto* font_id_data = static_cast<int32_t*>(duckdb_vector_get_data(v_font_id));
    auto* flags_data   = static_cast<int32_t*>(duckdb_vector_get_data(v_flags));

    idx_t row = 0;
    idx_t max_rows = 2048;
    while (row < max_rows) {
        const pdf_bboxes_font* f = pdf_bboxes_fonts_next(data->cursor);
        if (!f) break;

        font_id_data[row] = static_cast<int32_t>(f->font_id);
        flags_data[row]   = f->flags;
        duckdb_vector_assign_string_element(v_name,  row, f->name);
        duckdb_vector_assign_string_element(v_style, row, f->style);
        row++;
    }
    duckdb_data_chunk_set_size(output, row);
}

/* ── scalar JSON functions ───────────────────────────────────────── */

static const char* get_string(duckdb_vector vec, idx_t i) {
    auto* s = &static_cast<duckdb_string_t*>(duckdb_vector_get_data(vec))[i];
    return s->value.inlined.length > 12 ? s->value.pointer.ptr : s->value.inlined.inlined;
}

static int64_t get_bigint(duckdb_vector vec, idx_t i) {
    return static_cast<int64_t*>(duckdb_vector_get_data(vec))[i];
}

static void extract_json_scalar(duckdb_function_info info, duckdb_data_chunk input,
                                duckdb_vector output) {
    idx_t count = duckdb_data_chunk_get_size(input);
    idx_t ncols = duckdb_data_chunk_get_column_count(input);
    duckdb_vector v_path = duckdb_data_chunk_get_vector(input, 0);

    for (idx_t i = 0; i < count; i++) {
        const char* path = get_string(v_path, i);
        int sp = (ncols > 1) ? static_cast<int>(get_bigint(duckdb_data_chunk_get_vector(input, 1), i)) : 0;
        int ep = (ncols > 2) ? static_cast<int>(get_bigint(duckdb_data_chunk_get_vector(input, 2), i)) : 0;

        auto buf = read_file(path);
        std::string result = "[";
        if (!buf.empty()) {
            auto* cur = pdf_bboxes_extract_open(buf.data(), buf.size(), nullptr, sp, ep);
            if (cur) {
                bool first = true;
                while (const char* json = pdf_bboxes_extract_next_json(cur)) {
                    if (!first) result += ',';
                    result += json;
                    first = false;
                }
                pdf_bboxes_extract_close(cur);
            }
        }
        result += ']';
        duckdb_vector_assign_string_element_len(output, i, result.c_str(), result.size());
    }
}

static void fonts_json_scalar(duckdb_function_info info, duckdb_data_chunk input,
                              duckdb_vector output) {
    idx_t count = duckdb_data_chunk_get_size(input);
    duckdb_vector v_path = duckdb_data_chunk_get_vector(input, 0);

    for (idx_t i = 0; i < count; i++) {
        const char* path = get_string(v_path, i);
        auto buf = read_file(path);
        std::string result = "[";
        if (!buf.empty()) {
            auto* cur = pdf_bboxes_fonts_open(buf.data(), buf.size(), nullptr);
            if (cur) {
                bool first = true;
                while (const char* json = pdf_bboxes_fonts_next_json(cur)) {
                    if (!first) result += ',';
                    result += json;
                    first = false;
                }
                pdf_bboxes_fonts_close(cur);
            }
        }
        result += ']';
        duckdb_vector_assign_string_element_len(output, i, result.c_str(), result.size());
    }
}

/* ── registration ────────────────────────────────────────────────── */

static void register_extract(duckdb_connection conn) {
    duckdb_table_function func = duckdb_create_table_function();
    duckdb_table_function_set_name(func, "pdf_extract");
    duckdb_logical_type t = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    duckdb_table_function_add_parameter(func, t);
    duckdb_destroy_logical_type(&t);
    duckdb_table_function_set_bind(func, extract_bind);
    duckdb_table_function_set_init(func, extract_init);
    duckdb_table_function_set_function(func, extract_func);
    duckdb_register_table_function(conn, func);
    duckdb_destroy_table_function(&func);
}

static void register_fonts(duckdb_connection conn) {
    duckdb_table_function func = duckdb_create_table_function();
    duckdb_table_function_set_name(func, "pdf_fonts");
    duckdb_logical_type t = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    duckdb_table_function_add_parameter(func, t);
    duckdb_destroy_logical_type(&t);
    duckdb_table_function_set_bind(func, fonts_bind);
    duckdb_table_function_set_init(func, fonts_init);
    duckdb_table_function_set_function(func, fonts_func);
    duckdb_register_table_function(conn, func);
    duckdb_destroy_table_function(&func);
}

static void register_extract_json(duckdb_connection conn) {
    duckdb_scalar_function func = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(func, "pdf_extract_json");

    duckdb_logical_type t_str = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    duckdb_logical_type t_big = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
    duckdb_scalar_function_add_parameter(func, t_str);
    duckdb_scalar_function_set_varargs(func, t_big);
    duckdb_scalar_function_set_return_type(func, t_str);
    duckdb_destroy_logical_type(&t_str);
    duckdb_destroy_logical_type(&t_big);

    duckdb_scalar_function_set_function(func, extract_json_scalar);
    duckdb_register_scalar_function(conn, func);
    duckdb_destroy_scalar_function(&func);
}

static void register_fonts_json(duckdb_connection conn) {
    duckdb_scalar_function func = duckdb_create_scalar_function();
    duckdb_scalar_function_set_name(func, "pdf_fonts_json");

    duckdb_logical_type t_str = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    duckdb_scalar_function_add_parameter(func, t_str);
    duckdb_scalar_function_set_return_type(func, t_str);
    duckdb_destroy_logical_type(&t_str);

    duckdb_scalar_function_set_function(func, fonts_json_scalar);
    duckdb_register_scalar_function(conn, func);
    duckdb_destroy_scalar_function(&func);
}

/* ── entrypoint ──────────────────────────────────────────────────── */

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_connection connection, duckdb_extension_info info,
                            struct duckdb_extension_access* access) {
    pdf_bboxes_init();
    register_extract(connection);
    register_fonts(connection);
    register_extract_json(connection);
    register_fonts_json(connection);
    return true;
}
