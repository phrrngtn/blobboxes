#include "bboxes_types.h"

#include <miniz.h>
#include <pugixml.hpp>

#include <string>
#include <vector>
#include <cstring>

/* ── helpers ─────────────────────────────────────────────────────── */

/* extract a file from a zip archive in memory */
static std::vector<char> zip_extract(const void* buf, size_t len, const char* name) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, buf, len, 0))
        return {};

    int idx = mz_zip_reader_locate_file(&zip, name, nullptr, 0);
    if (idx < 0) {
        mz_zip_reader_end(&zip);
        return {};
    }

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip, idx, &stat)) {
        mz_zip_reader_end(&zip);
        return {};
    }

    std::vector<char> out(static_cast<size_t>(stat.m_uncomp_size));
    if (!mz_zip_reader_extract_to_mem(&zip, idx, out.data(), out.size(), 0))
        out.clear();

    mz_zip_reader_end(&zip);
    return out;
}

/* recursively gather all <w:t> text under a node — catches runs nested in
   <w:hyperlink>, <w:smartTag>, <w:ins>, etc., not just direct <w:r> children.
   <w:tab>/<w:br>/<w:cr> render as whitespace so words don't run together. */
static void gather_text(const pugi::xml_node& n, std::string& out) {
    for (auto& child : n.children()) {
        const char* nm = child.name();
        if (strcmp(nm, "w:t") == 0)        out += child.child_value();
        else if (strcmp(nm, "w:tab") == 0) out += '\t';
        else if (strcmp(nm, "w:br") == 0 || strcmp(nm, "w:cr") == 0) out += '\n';
        else                               gather_text(child, out);
    }
}

/* full text of a paragraph */
static std::string get_para_text(const pugi::xml_node& para) {
    std::string text; gather_text(para, text); return text;
}

/* get text from a table cell (may contain multiple paragraphs) */
static std::string get_cell_text(const pugi::xml_node& tc) {
    std::string text;
    bool first = true;
    for (auto& child : tc.children()) {
        if (strcmp(child.name(), "w:p") == 0) {
            if (!first) text += '\n';
            text += get_para_text(child);
            first = false;
        }
    }
    return text;
}

/* get integer attribute from <w:gridSpan w:val="N"> etc. */
static int get_span(const pugi::xml_node& tcPr, const char* elem, int def) {
    auto node = tcPr.child(elem);
    if (!node) return def;
    int v = node.attribute("w:val").as_int(def);
    return v > 0 ? v : def;
}

/* ── extract ─────────────────────────────────────────────────────── */

BBoxResult extract_docx(const void* buf, size_t len) {
    BBoxResult result;
    result.source_type = "docx";

    /* extract word/document.xml from the zip */
    auto xml_data = zip_extract(buf, len, "word/document.xml");
    if (xml_data.empty()) {
        result.page_count = -1;
        return result;
    }

    pugi::xml_document doc;
    pugi::xml_parse_result pr = doc.load_buffer(xml_data.data(), xml_data.size());
    if (!pr) {
        result.page_count = -1;
        return result;
    }

    /* default font + style */
    uint32_t font_id = result.fonts.intern("default");
    uint32_t style_id = result.styles.intern(
        font_id, BBOXES_DEFAULT_FONT_SIZE, BBOXES_DEFAULT_COLOR,
        BBOXES_DEFAULT_WEIGHT, false, false);

    /* Walk the body in DOCUMENT ORDER (y = reading-order line, monotonic).
       docx stores no geometry, so we use one flow of lines on a single page:
         - a paragraph is a full text span, IDENTICAL to the text reader's model
           (bboxes_text.cpp): x=1, w=len(text), h=1 — docx flow == text flow.
         - a table row subdivides its line into a real grid: x=col, w=colspan.
       Tables are the only genuine 2D structure; paragraphs are flow spans. */
    auto body = doc.child("w:document").child("w:body");
    if (!body) {
        result.page_count = -1;
        return result;
    }

    Page page;
    page.page_id     = 0;
    page.document_id = 0;
    page.page_number = 1;

    uint32_t line = 0;
    double max_x = 0.0;

    for (auto& blk : body.children()) {
        const char* nm = blk.name();

        if (strcmp(nm, "w:p") == 0) {
            std::string text = get_para_text(blk);
            if (text.empty()) continue;            /* no text to box */
            line++;
            BBox bb;
            bb.page_id  = 0;
            bb.style_id = style_id;
            bb.x = 1.0;
            bb.y = static_cast<double>(line);
            bb.w = static_cast<double>(text.size());   /* len(text), like bb_text */
            bb.h = 1.0;
            bb.text = std::move(text);
            if (bb.x + bb.w - 1 > max_x) max_x = bb.x + bb.w - 1;
            page.bboxes.push_back(std::move(bb));

        } else if (strcmp(nm, "w:tbl") == 0) {
            for (auto& tr : blk.children()) {
                if (strcmp(tr.name(), "w:tr") != 0) continue;
                line++;
                uint32_t col_num = 0;

                for (auto& tc : tr.children()) {
                    if (strcmp(tc.name(), "w:tc") != 0) continue;
                    col_num++;

                    auto tcPr = tc.child("w:tcPr");
                    int colspan = get_span(tcPr, "w:gridSpan", 1);

                    /* vMerge continuation cell: skip, but hold the column slot */
                    auto vMerge = tcPr.child("w:vMerge");
                    if (vMerge && !vMerge.attribute("w:val")) {
                        col_num += (colspan - 1);
                        continue;
                    }

                    BBox bb;
                    bb.page_id  = 0;
                    bb.style_id = style_id;
                    bb.x = static_cast<double>(col_num);
                    bb.y = static_cast<double>(line);
                    bb.w = static_cast<double>(colspan);
                    bb.h = 1.0;
                    bb.text = get_cell_text(tc);
                    if (bb.x + bb.w - 1 > max_x) max_x = bb.x + bb.w - 1;
                    page.bboxes.push_back(std::move(bb));

                    col_num += (colspan - 1);
                }
            }
        }
        /* else: w:sectPr and other block elements carry no text — ignore */
    }

    page.width  = max_x;
    page.height = static_cast<double>(line);
    result.page_count = 1;
    result.pages.push_back(std::move(page));

    return result;
}
