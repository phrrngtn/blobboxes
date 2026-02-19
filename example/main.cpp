#include "pdf_bboxes.h"
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

    pdf_bboxes_init();

    /* struct interface */
    printf("--- fonts (struct) ---\n");
    auto* fc = pdf_bboxes_fonts_open(buf.data(), buf.size(), nullptr);
    while (auto* font = pdf_bboxes_fonts_next(fc))
        printf("  [%u] %s style=%s flags=%d\n", font->font_id, font->name, font->style, font->flags);
    pdf_bboxes_fonts_close(fc);

    printf("--- bboxes (struct, first 5) ---\n");
    auto* ec = pdf_bboxes_extract_open(buf.data(), buf.size(), nullptr, 0, 0);
    int count = 0;
    while (auto* r = pdf_bboxes_extract_next(ec)) {
        printf("  p%d (%.1f,%.1f %.1fx%.1f) font=%u size=%.0f %s %s %s\n",
               r->page, r->x, r->y, r->w, r->h,
               r->font_id, r->font_size, r->style, r->color, r->text);
        if (++count >= 5) break;
    }
    pdf_bboxes_extract_close(ec);

    /* JSON interface */
    printf("--- fonts (json) ---\n");
    fc = pdf_bboxes_fonts_open(buf.data(), buf.size(), nullptr);
    while (auto* json = pdf_bboxes_fonts_next_json(fc))
        puts(json);
    pdf_bboxes_fonts_close(fc);

    printf("--- bboxes (json, first 5) ---\n");
    ec = pdf_bboxes_extract_open(buf.data(), buf.size(), nullptr, 0, 0);
    count = 0;
    while (auto* json = pdf_bboxes_extract_next_json(ec)) {
        puts(json);
        if (++count >= 5) break;
    }
    pdf_bboxes_extract_close(ec);

    pdf_bboxes_destroy();
    return 0;
}
