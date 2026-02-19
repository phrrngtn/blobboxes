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

    printf("--- fonts ---\n");
    auto* fc = pdf_bboxes_fonts_open(buf.data(), buf.size(), nullptr);
    while (auto* json = pdf_bboxes_fonts_next(fc))
        puts(json);
    pdf_bboxes_fonts_close(fc);

    printf("--- bboxes ---\n");
    auto* ec = pdf_bboxes_extract_open(buf.data(), buf.size(), nullptr, 0, 0);
    while (auto* json = pdf_bboxes_extract_next(ec))
        puts(json);
    pdf_bboxes_extract_close(ec);

    pdf_bboxes_destroy();
    return 0;
}
