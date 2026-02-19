#include "bboxes_types.h"

#include <sqlite3.h>
#include <string>
#include <vector>

BBoxResult extract_sql(const char* db_path, const char* query) {
    BBoxResult result;
    result.source_type = "sql";

    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        result.page_count = -1;
        return result;
    }

    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        result.page_count = -1;
        return result;
    }

    int col_count = sqlite3_column_count(stmt);

    /* one font per column (column name as font name) */
    for (int c = 0; c < col_count; c++) {
        const char* name = sqlite3_column_name(stmt, c);
        result.fonts.intern(name ? name : "");
    }

    /* one style per column */
    for (int c = 0; c < col_count; c++) {
        result.styles.intern(
            static_cast<uint32_t>(c), 12.0,
            "rgba(0,0,0,255)", "normal", false, false);
    }

    /* execute and collect rows */
    Page page;
    page.page_id     = 0;
    page.document_id = 0;
    page.page_number = 1;
    page.width       = static_cast<double>(col_count);

    uint32_t row_num = 0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        row_num++;
        for (int c = 0; c < col_count; c++) {
            const char* val = reinterpret_cast<const char*>(
                sqlite3_column_text(stmt, c));

            BBox bb;
            bb.page_id  = 0;
            bb.style_id = static_cast<uint32_t>(c);
            bb.x = static_cast<double>(c + 1);
            bb.y = static_cast<double>(row_num);
            bb.w = 1.0;
            bb.h = 1.0;
            bb.text = val ? val : "";
            page.bboxes.push_back(std::move(bb));
        }
    }

    page.height = static_cast<double>(row_num);
    result.page_count = 1;
    result.pages.push_back(std::move(page));

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return result;
}
