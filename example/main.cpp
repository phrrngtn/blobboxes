#include "bboxes.h"
#include <cstdio>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.pdf>\n", argv[0]);
        return 1;
    }

    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    std::vector<char> buf(sz);
    if (fread(buf.data(), 1, sz, f) != static_cast<size_t>(sz)) {
        fclose(f);
        fprintf(stderr, "read error\n");
        return 1;
    }
    fclose(f);

    bboxes_pdf_init();

    auto* cur = bboxes_open_pdf(buf.data(), buf.size(), nullptr, 0, 0);
    if (!cur) {
        fprintf(stderr, "failed to parse PDF\n");
        return 1;
    }

    /* doc */
    printf("--- doc ---\n");
    auto* doc = bboxes_get_doc(cur);
    printf("  source=%s pages=%d\n", doc->source_type, doc->page_count);

    /* pages */
    printf("--- pages ---\n");
    while (auto* p = bboxes_next_page(cur))
        printf("  page %d: %.0fx%.0f\n", p->page_number, p->width, p->height);

    /* fonts */
    printf("--- fonts ---\n");
    while (auto* font = bboxes_next_font(cur))
        printf("  [%u] %s\n", font->font_id, font->name);

    /* styles */
    printf("--- styles ---\n");
    while (auto* s = bboxes_next_style(cur))
        printf("  [%u] font=%u size=%.0f %s %s italic=%d\n",
               s->style_id, s->font_id, s->font_size, s->weight, s->color, s->italic);

    /* bboxes (first 10) */
    printf("--- bboxes (first 10) ---\n");
    int count = 0;
    while (auto* b = bboxes_next_bbox(cur)) {
        printf("  [%u] page=%u style=%u (%.1f,%.1f %.1fx%.1f) %s\n",
               b->bbox_id, b->page_id, b->style_id,
               b->x, b->y, b->w, b->h, b->text);
        if (++count >= 10) break;
    }

    bboxes_close(cur);
    bboxes_pdf_destroy();
    return 0;
}
