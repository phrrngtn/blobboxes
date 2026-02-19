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

static int64_t get_bigint(duckdb_vector vec, idx_t i) {
    return static_cast<int64_t*>(duckdb_vector_get_data(vec))[i];
}

/* ── shared bind/init helpers ────────────────────────────────────── */

struct BindData { char* file_path; };

struct InitData {
    std::vector<char> buf;
    bboxes_cursor* cursor;
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

static InitData* shared_init_cursor(duckdb_init_info info) {
    auto* bind = static_cast<BindData*>(duckdb_init_get_bind_data(info));
    auto* data = new InitData{};
    data->buf = read_file(bind->file_path);
    if (data->buf.empty()) {
        duckdb_init_set_error(info, "failed to read file");
        delete data;
        return nullptr;
    }
    data->cursor = bboxes_open_pdf(data->buf.data(), data->buf.size(), nullptr, 0, 0);
    if (!data->cursor) {
        duckdb_init_set_error(info, "failed to parse PDF");
        delete data;
        return nullptr;
    }
    duckdb_init_set_max_threads(info, 1);
    duckdb_init_set_init_data(info, data, [](void* p) {
        auto* d = static_cast<InitData*>(p);
        if (d->cursor) bboxes_close(d->cursor);
        delete d;
    });
    return data;
}

/* ── bboxes_doc table function ───────────────────────────────────── */

static void doc_bind(duckdb_bind_info info) {
    BindData* data;
    shared_bind_path(info, &data);

    duckdb_logical_type t_int = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
    duckdb_logical_type t_str = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);

    duckdb_bind_add_result_column(info, "document_id", t_int);
    duckdb_bind_add_result_column(info, "source_type", t_str);
    duckdb_bind_add_result_column(info, "filename", t_str);
    duckdb_bind_add_result_column(info, "page_count", t_int);

    duckdb_destroy_logical_type(&t_int);
    duckdb_destroy_logical_type(&t_str);
}

static void doc_init(duckdb_init_info info) { shared_init_cursor(info); }

static void doc_func(duckdb_function_info info, duckdb_data_chunk output) {
    auto* data = static_cast<InitData*>(duckdb_function_get_init_data(info));
    const bboxes_doc* d = bboxes_get_doc(data->cursor);
    if (!d) { duckdb_data_chunk_set_size(output, 0); return; }

    auto* doc_id_data = static_cast<int32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 0)));
    auto* page_count_data = static_cast<int32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 3)));

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
    page_count_data[0] = d->page_count;

    duckdb_data_chunk_set_size(output, 1);
    /* Ensure subsequent calls return 0 rows */
    bboxes_close(data->cursor);
    data->cursor = nullptr;
}

/* ── bboxes_pages table function ─────────────────────────────────── */

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

static void pages_init(duckdb_init_info info) { shared_init_cursor(info); }

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

/* ── bboxes_fonts table function ─────────────────────────────────── */

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

static void fonts_init(duckdb_init_info info) { shared_init_cursor(info); }

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

/* ── bboxes_styles table function ────────────────────────────────── */

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

static void styles_init(duckdb_init_info info) { shared_init_cursor(info); }

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

/* ── bboxes table function ───────────────────────────────────────── */

static void bboxes_bind(duckdb_bind_info info) {
    BindData* data;
    shared_bind_path(info, &data);

    duckdb_logical_type t_int = duckdb_create_logical_type(DUCKDB_TYPE_INTEGER);
    duckdb_logical_type t_dbl = duckdb_create_logical_type(DUCKDB_TYPE_DOUBLE);
    duckdb_logical_type t_str = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);

    duckdb_bind_add_result_column(info, "bbox_id", t_int);
    duckdb_bind_add_result_column(info, "page_id", t_int);
    duckdb_bind_add_result_column(info, "style_id", t_int);
    duckdb_bind_add_result_column(info, "x", t_dbl);
    duckdb_bind_add_result_column(info, "y", t_dbl);
    duckdb_bind_add_result_column(info, "w", t_dbl);
    duckdb_bind_add_result_column(info, "h", t_dbl);
    duckdb_bind_add_result_column(info, "text", t_str);

    duckdb_destroy_logical_type(&t_int);
    duckdb_destroy_logical_type(&t_dbl);
    duckdb_destroy_logical_type(&t_str);
}

static void bboxes_init_fn(duckdb_init_info info) { shared_init_cursor(info); }

static void bboxes_func(duckdb_function_info info, duckdb_data_chunk output) {
    auto* data = static_cast<InitData*>(duckdb_function_get_init_data(info));
    if (!data->cursor) { duckdb_data_chunk_set_size(output, 0); return; }

    auto* bbox_id_data  = static_cast<int32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 0)));
    auto* page_id_data  = static_cast<int32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 1)));
    auto* style_id_data = static_cast<int32_t*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 2)));
    auto* x_data = static_cast<double*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 3)));
    auto* y_data = static_cast<double*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 4)));
    auto* w_data = static_cast<double*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 5)));
    auto* h_data = static_cast<double*>(duckdb_vector_get_data(duckdb_data_chunk_get_vector(output, 6)));
    duckdb_vector v_text = duckdb_data_chunk_get_vector(output, 7);

    idx_t row = 0;
    while (row < 2048) {
        const bboxes_bbox* b = bboxes_next_bbox(data->cursor);
        if (!b) break;
        bbox_id_data[row]  = static_cast<int32_t>(b->bbox_id);
        page_id_data[row]  = static_cast<int32_t>(b->page_id);
        style_id_data[row] = static_cast<int32_t>(b->style_id);
        x_data[row] = b->x;
        y_data[row] = b->y;
        w_data[row] = b->w;
        h_data[row] = b->h;
        duckdb_vector_assign_string_element(v_text, row, b->text);
        row++;
    }
    duckdb_data_chunk_set_size(output, row);
}

/* ── scalar JSON functions ───────────────────────────────────────── */

typedef const char* (*json_iter_fn)(bboxes_cursor*);

static void json_array_scalar(duckdb_function_info info, duckdb_data_chunk input,
                               duckdb_vector output, json_iter_fn iter_fn) {
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
            auto* cur = bboxes_open_pdf(buf.data(), buf.size(), nullptr, sp, ep);
            if (cur) {
                bool first = true;
                while (const char* json = iter_fn(cur)) {
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

static void json_single_scalar(duckdb_function_info info, duckdb_data_chunk input,
                                duckdb_vector output,
                                const char* (*get_fn)(bboxes_cursor*)) {
    idx_t count = duckdb_data_chunk_get_size(input);
    idx_t ncols = duckdb_data_chunk_get_column_count(input);
    duckdb_vector v_path = duckdb_data_chunk_get_vector(input, 0);

    for (idx_t i = 0; i < count; i++) {
        const char* path = get_string(v_path, i);
        int sp = (ncols > 1) ? static_cast<int>(get_bigint(duckdb_data_chunk_get_vector(input, 1), i)) : 0;
        int ep = (ncols > 2) ? static_cast<int>(get_bigint(duckdb_data_chunk_get_vector(input, 2), i)) : 0;

        auto buf = read_file(path);
        std::string result = "null";
        if (!buf.empty()) {
            auto* cur = bboxes_open_pdf(buf.data(), buf.size(), nullptr, sp, ep);
            if (cur) {
                const char* json = get_fn(cur);
                if (json) result = json;
                bboxes_close(cur);
            }
        }
        duckdb_vector_assign_string_element_len(output, i, result.c_str(), result.size());
    }
}

static void doc_json_scalar(duckdb_function_info info, duckdb_data_chunk input,
                             duckdb_vector output) {
    json_single_scalar(info, input, output, bboxes_get_doc_json);
}

static void pages_json_scalar(duckdb_function_info info, duckdb_data_chunk input,
                               duckdb_vector output) {
    json_array_scalar(info, input, output, bboxes_next_page_json);
}

static void fonts_json_scalar(duckdb_function_info info, duckdb_data_chunk input,
                               duckdb_vector output) {
    json_array_scalar(info, input, output, bboxes_next_font_json);
}

static void styles_json_scalar(duckdb_function_info info, duckdb_data_chunk input,
                                duckdb_vector output) {
    json_array_scalar(info, input, output, bboxes_next_style_json);
}

static void bboxes_json_scalar(duckdb_function_info info, duckdb_data_chunk input,
                                duckdb_vector output) {
    json_array_scalar(info, input, output, bboxes_next_bbox_json);
}

/* ── registration helpers ────────────────────────────────────────── */

static void register_table_fn(duckdb_connection conn, const char* name,
                               duckdb_table_function_bind_t bind_fn,
                               duckdb_table_function_init_t init_fn,
                               duckdb_table_function_t func_fn) {
    duckdb_table_function func = duckdb_create_table_function();
    duckdb_table_function_set_name(func, name);
    duckdb_logical_type t = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
    duckdb_table_function_add_parameter(func, t);
    duckdb_destroy_logical_type(&t);
    duckdb_table_function_set_bind(func, bind_fn);
    duckdb_table_function_set_init(func, init_fn);
    duckdb_table_function_set_function(func, func_fn);
    duckdb_register_table_function(conn, func);
    duckdb_destroy_table_function(&func);
}

static void register_json_scalar(duckdb_connection conn, const char* name,
                                  duckdb_scalar_function_t func_fn, bool varargs) {
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

    duckdb_scalar_function_set_function(func, func_fn);
    duckdb_register_scalar_function(conn, func);
    duckdb_destroy_scalar_function(&func);
}

/* ── entrypoint ──────────────────────────────────────────────────── */

DUCKDB_EXTENSION_ENTRYPOINT(duckdb_connection connection, duckdb_extension_info info,
                            struct duckdb_extension_access* access) {
    bboxes_pdf_init();

    register_table_fn(connection, "bboxes_doc",    doc_bind,    doc_init,       doc_func);
    register_table_fn(connection, "bboxes_pages",  pages_bind,  pages_init,     pages_func);
    register_table_fn(connection, "bboxes_fonts",  fonts_bind,  fonts_init,     fonts_func);
    register_table_fn(connection, "bboxes_styles", styles_bind, styles_init,    styles_func);
    register_table_fn(connection, "bboxes",        bboxes_bind, bboxes_init_fn, bboxes_func);

    register_json_scalar(connection, "bboxes_doc_json",    doc_json_scalar,    true);
    register_json_scalar(connection, "bboxes_pages_json",  pages_json_scalar,  true);
    register_json_scalar(connection, "bboxes_fonts_json",  fonts_json_scalar,  true);
    register_json_scalar(connection, "bboxes_styles_json", styles_json_scalar, true);
    register_json_scalar(connection, "bboxes_json",        bboxes_json_scalar, true);

    return true;
}
