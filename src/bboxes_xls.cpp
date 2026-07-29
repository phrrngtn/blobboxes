/* Legacy .xls (BIFF5/BIFF8, OLE2/CFB) backend via libxls.
 *
 * Parallel to bboxes_xlsx.cpp (xlnt / OOXML). libxls does the OLE2/CFB + BIFF parsing
 * blobboxes deliberately keeps out of its own tree; we map its cells to the SAME BBox grid
 * (x=col, y=row, 1-based, matching the xlsx reader). Fidelity note: unlike the xlsx path we do
 * NOT recover shared/array-formula regions (BIFF stores formula bytecode; libxls evaluates it
 * to a result rather than exposing the source), so `formula` is left empty — cells/values/merges
 * are first-class, drag-fill regions are not. That is exactly the tier the NHS-style .xls files need.
 */
#include "bboxes.h"
#include "bboxes_types.h"

#include <xls.h>
#include <cstdio>
#include <string>
#include <unordered_map>

using namespace xls;   // libxls wraps its C API in `namespace xls` (see xls.h)

BBoxResult extract_xls(const void* buf, size_t len, const char* /*password*/,
                       int start_page, int end_page) {
    BBoxResult res;
    res.source_type = "xls";
    res.page_count  = 0;

    xls_error_t err = LIBXLS_OK;
    xlsWorkBook* wb = xls_open_buffer(static_cast<const unsigned char*>(buf), len, "UTF-8", &err);
    if (!wb) { res.page_count = -1; return res; }   /* wrap_result() turns <0 into a NULL cursor */

    /* Decode BIFF XF -> FONT into the bbox StyleTable, cached per XF index. Font name / size (twips->pt) /
       bold / italic / underline are recovered; color is left default (BIFF palette-index decode is a
       separate task). Fonts intern by name, styles by (font,size,color,weight,italic,underline). */
    std::unordered_map<uint16_t, uint32_t> xf_to_style;
    auto style_for = [&](uint16_t xfidx) -> uint32_t {
        auto it = xf_to_style.find(xfidx);
        if (it != xf_to_style.end()) return it->second;
        std::string name = "default", weight = BBOXES_DEFAULT_WEIGHT, color = BBOXES_DEFAULT_COLOR;
        double size = BBOXES_DEFAULT_FONT_SIZE; bool italic = false, underline = false;
        if (xfidx < wb->xfs.count && wb->xfs.xf) {
            uint16_t f = wb->xfs.xf[xfidx].font;
            uint32_t fpos = (f > 4) ? static_cast<uint32_t>(f) - 1 : f;   /* BIFF skips font index 4 */
            if (fpos < wb->fonts.count && wb->fonts.font) {
                const auto& fo = wb->fonts.font[fpos];
                if (fo.name && fo.name[0]) name = fo.name;
                if (fo.height) size = fo.height / 20.0;                   /* twips -> points */
                if (fo.bold >= 700 || (fo.flag & 0x0001)) weight = "bold";
                italic    = (fo.flag & 0x0002) != 0;
                underline = (fo.underline != 0);
            }
        }
        uint32_t fid = res.fonts.intern(name.c_str());
        uint32_t sid = res.styles.intern(fid, size, color, weight, italic, underline);
        xf_to_style[xfidx] = sid;
        return sid;
    };

    const int nsheets = static_cast<int>(wb->sheets.count);
    for (int s = 0; s < nsheets; s++) {
        if (start_page > 0 && (s + 1) < start_page) continue;
        if (end_page   > 0 && (s + 1) > end_page)   continue;
        xlsWorkSheet* ws = xls_getWorkSheet(wb, s);
        if (!ws || xls_parseWorkSheet(ws) != LIBXLS_OK) continue;

        Page page;
        page.page_id     = static_cast<uint32_t>(s);
        page.document_id = 0;
        page.page_number = s + 1;
        page.width       = static_cast<double>(ws->rows.lastcol + 1);
        page.height      = static_cast<double>(ws->rows.lastrow + 1);

        for (WORD r = 0; r <= ws->rows.lastrow; r++) {
            for (WORD c = 0; c <= ws->rows.lastcol; c++) {
                xlsCell* cell = xls_cell(ws, r, c);
                if (!cell || cell->isHidden) continue;
                const bool has_str = cell->str && cell->str[0] != '\0';
                const WORD id = cell->id;
                const bool numeric = (id == XLS_RECORD_NUMBER || id == XLS_RECORD_RK || id == XLS_RECORD_MULRK);
                const bool boolerr = (id == XLS_RECORD_BOOLERR);
                const bool formula = (id == XLS_RECORD_FORMULA);
                if (!has_str && !numeric && !boolerr && !formula) continue;   /* blank / nothing to emit */

                BBox b;
                b.page_id  = page.page_id;
                b.style_id = style_for(cell->xf);
                b.x = static_cast<double>(cell->col + 1);   /* 1-based col, matching the xlsx reader */
                b.y = static_cast<double>(cell->row + 1);   /* 1-based row */
                b.w = static_cast<double>(cell->colspan ? cell->colspan : 1);
                b.h = static_cast<double>(cell->rowspan ? cell->rowspan : 1);
                b.text = has_str ? std::string(cell->str) : std::string();

                if (numeric) {
                    b.cell_type = BBOX_NUMBER; b.vnum = cell->d;
                    if (b.text.empty()) { char t[32]; std::snprintf(t, sizeof t, "%.15g", cell->d); b.text = t; }
                } else if (boolerr) {
                    if (has_str && cell->str[0] == '#') { b.cell_type = BBOX_ERROR; }
                    else { b.cell_type = BBOX_BOOL; b.vbool = (cell->d != 0.0);
                           if (b.text.empty()) b.text = b.vbool ? "TRUE" : "FALSE"; }
                } else if (formula) {
                    /* libxls evaluates: a numeric result leaves str empty (value in d), a string
                       result populates str. No source formula bytes are exposed -> formula stays empty. */
                    if (has_str) { b.cell_type = BBOX_STRING; }
                    else { b.cell_type = BBOX_NUMBER; b.vnum = cell->d;
                           char t[32]; std::snprintf(t, sizeof t, "%.15g", cell->d); b.text = t; }
                } else {
                    b.cell_type = BBOX_STRING;   /* LABEL / LABELSST */
                }

                if (cell->colspan > 1 || cell->rowspan > 1) {
                    page.merges.push_back({ cell->row + 1, cell->col + 1,
                        cell->row + (cell->rowspan ? cell->rowspan : 1),
                        cell->col + (cell->colspan ? cell->colspan : 1) - 1 });
                }
                page.bboxes.push_back(std::move(b));
            }
        }
        res.pages.push_back(std::move(page));
        res.page_count++;
    }
    xls_close(wb);
    return res;
}
