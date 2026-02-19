#include "pdf_bboxes.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

static int print_json(const char* json, void* /*user_data*/) {
    puts(json);
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.pdf>\n", argv[0]);
        return 1;
    }

    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
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
    pdf_bboxes_fonts(buf.data(), buf.size(), nullptr, print_json, nullptr);

    printf("--- bboxes ---\n");
    pdf_bboxes_extract(buf.data(), buf.size(), nullptr, print_json, nullptr);

    pdf_bboxes_destroy();
    return 0;
}
