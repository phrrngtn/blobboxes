#include "bboxes_types.h"

#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>

#include <string>
#include <cstring>
#include <cstdlib>
#include <cctype>

/* ── helpers ─────────────────────────────────────────────────────── */

/* case-sensitive local-name compare (HTML tag names are lowercased by lexbor) */
static bool name_is(lxb_dom_node_t* n, const char* want) {
    if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) return false;
    size_t len;
    const lxb_char_t* nm = lxb_dom_element_local_name(lxb_dom_interface_element(n), &len);
    return nm && len == std::strlen(want) && std::memcmp(nm, want, len) == 0;
}

/* Block-level flow elements that map to one reading-order line each. Emitting the
   block (not its inner runs) means text_content already gathers descendants, so we
   do NOT recurse into a matched flow block — no double-emit of nested paragraphs. */
static bool is_flow_block(lxb_dom_node_t* n) {
    static const char* const kFlow[] = {
        "p", "li", "h1", "h2", "h3", "h4", "h5", "h6",
        "blockquote", "pre", "dt", "dd", "figcaption", "caption",
    };
    for (const char* nm : kFlow)
        if (name_is(n, nm)) return true;
    return false;
}

/* whitespace-normalized text content of a node (runs of whitespace → single
   space, trimmed). The returned lexbor buffer is released explicitly. */
static std::string node_text(lxb_dom_node_t* n) {
    size_t len = 0;
    lxb_char_t* t = lxb_dom_node_text_content(n, &len);
    std::string out;
    if (t) {
        out.reserve(len);
        bool sp = false;
        for (size_t i = 0; i < len; i++) {
            char c = static_cast<char>(t[i]);
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!out.empty() && !sp) { out += ' '; sp = true; }
            } else {
                out += c; sp = false;
            }
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        lxb_dom_document_destroy_text(n->owner_document, t);
    }
    return out;
}

/* positive integer attribute (colspan/rowspan); returns `def` when absent/invalid */
static int span_attr(lxb_dom_node_t* n, const char* attr, int def) {
    size_t vlen = 0;
    const lxb_char_t* v = lxb_dom_element_get_attribute(
        lxb_dom_interface_element(n),
        reinterpret_cast<const lxb_char_t*>(attr), std::strlen(attr), &vlen);
    if (!v || vlen == 0) return def;
    int val = std::atoi(std::string(reinterpret_cast<const char*>(v), vlen).c_str());
    return val > 0 ? val : def;
}

/* ── DOM walk ────────────────────────────────────────────────────── */

/* Per-table cursor. `row` is seeded from the enclosing document row rather than
   from -1, so table rows continue the document's row numbering instead of
   restarting; `col` resets per row. See the note on flow_line below. */
struct TableCtx { int row; int col; };

struct WalkCtx {
    Page*    page;
    uint32_t style_id;
    /* The document row counter, shared by flow blocks AND tables.
     *
     * It used to be flow-only, with each <table> starting its own counter at 0.
     * That made y ambiguous: a heading after a table got the same y as a row
     * inside it, so ordering the document by (y, x) interleaved the two. Against
     * Chromium's rendered geometry that single collision produced 23 of 24
     * reading-order inversions; sharing one counter took Spearman rho from
     * 0.9552 to 0.9996. See experiments/html_grid_diagnose.py, which is the
     * regression test for this. */
    int      flow_line = 0;
    double   max_x = 0.0;     /* widest extent  (x + w) */
    double   max_y = 0.0;     /* tallest extent (y + h) */
};

/* Walk children in document order. `tc != nullptr` means we are inside a <table>.
   Imputed integer geometry (0-based), mirroring the proven reducer:
     - <table>: open a grid cursor seeded from the current document row, and
                descend; on the way out, resume flow numbering after the last
                row the table used.
     - <tr>:    advance row, reset col.
     - <td>/<th>: emit a cell box at (x=col, y=row, w=colspan, h=rowspan).
     - flow block (p/li/hN/…): emit a box at (x=depth, y=line++, w=len(text), h=1).
     - anything else: recurse, preserving the table cursor. */
static void walk(lxb_dom_node_t* node, int depth, WalkCtx* ctx, TableCtx* tc) {
    for (lxb_dom_node_t* n = node->first_child; n; n = n->next) {
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;

        /* skip non-document text: script/style/etc. carry code, not content */
        if (name_is(n, "script") || name_is(n, "style") ||
            name_is(n, "noscript") || name_is(n, "template")) continue;

        if (name_is(n, "table")) {
            /* Seed from the enclosing row so the table continues the document's
               numbering. The first <tr> bumps this, hence the -1. A nested
               table sits within its cell's row and must not disturb the
               document counter, so only a top-level table writes back. */
            const int base = tc ? tc->row : ctx->flow_line - 1;
            TableCtx t{base, 0};
            walk(n, depth + 1, ctx, &t);
            /* Resume after the last row the table actually used. An empty table
               emits no <tr>, leaves t.row == base, and so costs no rows. */
            if (!tc) ctx->flow_line = t.row + 1;

        } else if (tc && name_is(n, "tr")) {
            tc->row++;
            tc->col = 0;
            walk(n, depth + 1, ctx, tc);

        } else if (tc && (name_is(n, "td") || name_is(n, "th"))) {
            int colspan = span_attr(n, "colspan", 1);
            int rowspan = span_attr(n, "rowspan", 1);
            BBox bb;
            bb.page_id   = 0;
            bb.style_id  = ctx->style_id;
            bb.x = static_cast<double>(tc->col);
            bb.y = static_cast<double>(tc->row);
            bb.w = static_cast<double>(colspan);
            bb.h = static_cast<double>(rowspan);
            bb.cell_type = BBOX_STRING;
            bb.text = node_text(n);
            if (bb.x + bb.w > ctx->max_x) ctx->max_x = bb.x + bb.w;
            if (bb.y + bb.h > ctx->max_y) ctx->max_y = bb.y + bb.h;
            ctx->page->bboxes.push_back(std::move(bb));
            tc->col += colspan;

        } else if (is_flow_block(n)) {
            std::string text = node_text(n);
            if (text.empty()) continue;          /* nothing to box */
            BBox bb;
            bb.page_id   = 0;
            bb.style_id  = ctx->style_id;
            bb.x = static_cast<double>(depth);
            bb.y = static_cast<double>(ctx->flow_line++);
            bb.w = static_cast<double>(text.size());
            bb.h = 1.0;
            bb.cell_type = BBOX_STRING;
            bb.text = std::move(text);
            if (bb.x + bb.w > ctx->max_x) ctx->max_x = bb.x + bb.w;
            if (bb.y + bb.h > ctx->max_y) ctx->max_y = bb.y + bb.h;
            ctx->page->bboxes.push_back(std::move(bb));

        } else {
            walk(n, depth + 1, ctx, tc);
        }
    }
}

/* ── extract ─────────────────────────────────────────────────────── */

BBoxResult extract_html(const void* buf, size_t len) {
    BBoxResult result;
    result.source_type = "html";

    lxb_html_document_t* doc = lxb_html_document_create();
    if (!doc) { result.page_count = -1; return result; }

    lxb_status_t st = lxb_html_document_parse(
        doc, static_cast<const lxb_char_t*>(buf), len);
    if (st != LXB_STATUS_OK) {
        lxb_html_document_destroy(doc);
        result.page_count = -1;
        return result;
    }

    /* default font + style (HTML carries no typographic geometry here) */
    uint32_t font_id = result.fonts.intern("default");
    uint32_t style_id = result.styles.intern(
        font_id, BBOXES_DEFAULT_FONT_SIZE, BBOXES_DEFAULT_COLOR,
        BBOXES_DEFAULT_WEIGHT, false, false);

    Page page;
    page.page_id     = 0;
    page.document_id = 0;
    page.page_number = 1;

    WalkCtx ctx;
    ctx.page     = &page;
    ctx.style_id = style_id;

    /* Prefer <body>; fall back to the document root for body-less fragments. */
    lxb_dom_node_t* body = lxb_dom_interface_node(lxb_html_document_body_element(doc));
    lxb_dom_node_t* root = body ? body : lxb_dom_interface_node(doc);
    walk(root, 0, &ctx, nullptr);

    page.width  = ctx.max_x;
    page.height = ctx.max_y;
    result.page_count = 1;
    result.pages.push_back(std::move(page));

    lxb_html_document_destroy(doc);
    return result;
}
