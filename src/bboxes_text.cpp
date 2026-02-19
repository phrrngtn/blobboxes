#include "bboxes_types.h"

#include <string>
#include <cstring>

BBoxResult extract_text(const void* buf, size_t len) {
    BBoxResult result;
    result.source_type = "text";

    /* single font and style for plain text */
    uint32_t font_id = result.fonts.intern("monospace");
    uint32_t style_id = result.styles.intern(
        font_id, 12.0, "rgba(0,0,0,255)", "normal", false, false);

    const char* data = static_cast<const char*>(buf);
    const char* end = data + len;

    Page page;
    page.page_id     = 0;
    page.document_id = 0;
    page.page_number = 1;

    uint32_t bbox_id = 0;
    uint32_t line_number = 0;
    double max_width = 0.0;
    const char* line_start = data;

    while (line_start <= end) {
        const char* line_end = static_cast<const char*>(
            memchr(line_start, '\n', end - line_start));
        if (!line_end) line_end = end;

        size_t line_len = line_end - line_start;
        line_number++;

        /* skip empty lines (no bbox, but count for height) */
        if (line_len > 0) {
            BBox bb;
            bb.bbox_id  = bbox_id++;
            bb.page_id  = 0;
            bb.style_id = style_id;
            bb.x = 1.0;
            bb.y = static_cast<double>(line_number);
            bb.w = static_cast<double>(line_len);
            bb.h = 1.0;
            bb.text = std::string(line_start, line_len);
            page.bboxes.push_back(std::move(bb));

            if (static_cast<double>(line_len) > max_width)
                max_width = static_cast<double>(line_len);
        }

        if (line_end == end) break;
        line_start = line_end + 1;
    }

    page.width  = max_width;
    page.height = static_cast<double>(line_number);
    result.page_count = 1;
    result.pages.push_back(std::move(page));

    return result;
}
