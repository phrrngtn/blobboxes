// bench_reader.cpp — honest A/B of the cell-grid extraction, three tiers, all from the
// SAME in-memory .xlsx bytes (raw → records, inflate included in every tier):
//   1. workbook::load()            — full DOM, retains cell_map_ (heavy baseline)
//   2. streaming_workbook_reader   — xlnt pull, no retained grid (the FAIR baseline)
//   3. hand pull-scan (memmem)     — no xlnt objects at all
// Every tier PRODUCES the real records (owned strings per cell), not just a checksum,
// so materialization cost is counted equally. The decision number is scan ÷ streamR.
//
//   bench_reader <file.xlsx> [file2.xlsx ...]
#include <xlnt/xlnt.hpp>
#include <xlnt/workbook/streaming_workbook_reader.hpp>
#include <miniz.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;
static double ms(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

static std::vector<uint8_t> read_file(const char* path) {
    std::vector<uint8_t> buf;
    FILE* f = std::fopen(path, "rb");
    if (!f) return buf;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    buf.resize(n > 0 ? n : 0);
    if (n > 0 && std::fread(buf.data(), 1, n, f) != (size_t)n) buf.clear();
    std::fclose(f);
    return buf;
}

// The real output grain: one owned record per emitted cell (like BBox).
struct Cell { uint32_t r = 0, c = 0, s = 0; std::string value, formula; };
using Cells = std::vector<Cell>;

static uint64_t checksum(const Cells& v) {          // proves same work + same data extracted
    uint64_t h = v.size();
    for (auto& c : v) h += c.value.size() + c.formula.size() + c.r + c.c + c.s;
    return h;
}

// ---- Tier 1: xlnt full DOM load(), then iterate every cell ----
static Cells build_load(const std::vector<uint8_t>& bytes) {
    Cells out;
    xlnt::workbook wb;
    wb.load(bytes);
    for (auto ws : wb) {
        auto hr = ws.highest_row();
        auto hc = ws.highest_column();
        for (auto row = 1u; row <= hr; row++)
            for (auto col = xlnt::column_t(1); col <= hc; col++) {
                if (!ws.has_cell(xlnt::cell_reference(col, row))) continue;
                auto c = ws.cell(xlnt::cell_reference(col, row));
                if (!c.has_value() && !c.has_formula()) continue;
                Cell rec; rec.r = row; rec.c = col.index;
                rec.value = c.to_string();
                if (c.has_formula()) rec.formula = "=" + c.formula();
                out.push_back(std::move(rec));
            }
    }
    return out;
}

// ---- Tier 2: xlnt streaming_workbook_reader — pull, no retained cell_map_ ----
static Cells build_stream_xlnt(const std::vector<uint8_t>& bytes) {
    Cells out;
    xlnt::streaming_workbook_reader r;
    r.open(bytes);
    for (const auto& title : r.sheet_titles()) {
        r.begin_worksheet(title);
        while (r.has_cell()) {
            auto c = r.read_cell();
            if (!c.has_value() && !c.has_formula()) continue;
            Cell rec; rec.r = c.row(); rec.c = c.column().index;
            rec.value = c.to_string();
            if (c.has_formula()) rec.formula = "=" + c.formula();
            out.push_back(std::move(rec));
        }
        r.end_worksheet();
    }
    return out;
}

// ---- Tier 3: hand pull-scan the worksheet XML off the same zip, no xlnt objects ----
static bool zip_extract(mz_zip_archive& z, const char* name, std::string& out) {
    int idx = mz_zip_reader_locate_file(&z, name, nullptr, 0);
    if (idx < 0) return false;
    size_t sz = 0;
    void* p = mz_zip_reader_extract_to_heap(&z, idx, &sz, 0);
    if (!p) return false;
    out.assign(static_cast<char*>(p), sz);
    mz_free(p);
    return true;
}

static std::vector<std::string> parse_shared_strings(const std::string& xml) {
    std::vector<std::string> out;
    const char* p = xml.data();
    const char* end = p + xml.size();
    while ((p = static_cast<const char*>(memmem(p, end - p, "<si>", 4)))) {
        p += 4;
        const char* si_end = static_cast<const char*>(memmem(p, end - p, "</si>", 5));
        if (!si_end) break;
        std::string val;
        const char* q = p;
        while (q < si_end) {
            const char* t = static_cast<const char*>(memmem(q, si_end - q, "<t", 2));
            if (!t) break;
            const char* gt = static_cast<const char*>(std::memchr(t, '>', si_end - t));
            if (!gt) break;
            if (*(gt - 1) == '/') { q = gt + 1; continue; }
            const char* tc = static_cast<const char*>(memmem(gt, si_end - gt, "</t>", 4));
            if (!tc) break;
            val.append(gt + 1, tc - (gt + 1));
            q = tc + 4;
        }
        out.push_back(std::move(val));
        p = si_end + 5;
    }
    return out;
}

static std::string inner(const char* p, const char* cell_end, const char* open, size_t olen,
                         const char* close, size_t clen) {
    const char* t = static_cast<const char*>(memmem(p, cell_end - p, open, olen));
    if (!t) return {};
    const char* gt = static_cast<const char*>(std::memchr(t, '>', cell_end - t));
    if (!gt || *(gt - 1) == '/') return {};
    const char* c = static_cast<const char*>(memmem(gt, cell_end - gt, close, clen));
    if (!c) return {};
    return std::string(gt + 1, c - (gt + 1));
}

static std::string attr(const char* p, const char* tag_end, const char* name) {
    const char* a = static_cast<const char*>(memmem(p, tag_end - p, name, std::strlen(name)));
    if (!a) return {};
    a += std::strlen(name);
    const char* q = static_cast<const char*>(std::memchr(a, '"', tag_end - a));
    if (!q) return {};
    return std::string(a, q - a);
}

// "B12" -> (row 12, col 2). Handles the common r="AB123" form.
static void parse_ref(const std::string& ref, uint32_t& row, uint32_t& col) {
    col = 0; row = 0;
    size_t i = 0;
    for (; i < ref.size() && ref[i] >= 'A' && ref[i] <= 'Z'; i++) col = col * 26 + (ref[i] - 'A' + 1);
    for (; i < ref.size() && ref[i] >= '0' && ref[i] <= '9'; i++) row = row * 10 + (ref[i] - '0');
}

static void scan_sheet(const std::string& xml, const std::vector<std::string>& sst, Cells& out) {
    const char* p = xml.data();
    const char* end = p + xml.size();
    while ((p = static_cast<const char*>(memmem(p, end - p, "<c ", 3)))) {
        const char* tag_end = static_cast<const char*>(std::memchr(p, '>', end - p));
        if (!tag_end) break;
        bool self_closing = (*(tag_end - 1) == '/');
        std::string rattr = attr(p, tag_end, " r=\"");
        std::string sattr = attr(p, tag_end, " s=\"");
        std::string tattr = attr(p, tag_end, " t=\"");
        if (self_closing) { p = tag_end + 1; continue; }
        const char* cell_end = static_cast<const char*>(memmem(tag_end, end - tag_end, "</c>", 4));
        if (!cell_end) break;

        std::string formula = inner(tag_end, cell_end, "<f", 2, "</f>", 4);
        std::string v = (tattr == "inlineStr") ? inner(tag_end, cell_end, "<t", 2, "</t>", 4)
                                                : inner(tag_end, cell_end, "<v", 2, "</v>", 4);
        if (v.empty() && formula.empty()) { p = cell_end + 4; continue; }

        Cell rec;
        parse_ref(rattr, rec.r, rec.c);
        rec.s = sattr.empty() ? 0 : (uint32_t)std::strtoul(sattr.c_str(), nullptr, 10);  // missing s => 0
        if (tattr == "s") {                       // shared-string index -> resolve (owned copy)
            long i = std::strtol(v.c_str(), nullptr, 10);
            rec.value = (i >= 0 && (size_t)i < sst.size()) ? sst[i] : std::string();
        } else {
            rec.value = std::move(v);
        }
        if (!formula.empty()) rec.formula = "=" + formula;
        out.push_back(std::move(rec));
        p = cell_end + 4;
    }
}

static Cells build_scan(const std::vector<uint8_t>& bytes) {
    Cells out;
    mz_zip_archive z;
    std::memset(&z, 0, sizeof(z));
    if (!mz_zip_reader_init_mem(&z, bytes.data(), bytes.size(), 0)) return out;

    std::string sstxml;
    std::vector<std::string> sst;
    if (zip_extract(z, "xl/sharedStrings.xml", sstxml)) sst = parse_shared_strings(sstxml);

    int n = (int)mz_zip_reader_get_num_files(&z);
    for (int i = 0; i < n; i++) {
        char name[512];
        mz_zip_reader_get_filename(&z, i, name, sizeof(name));
        if (std::strncmp(name, "xl/worksheets/sheet", 19) != 0 || !std::strstr(name, ".xml")) continue;
        size_t sz = 0;
        void* pd = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
        if (!pd) continue;
        std::string sheet(static_cast<char*>(pd), sz);
        mz_free(pd);
        scan_sheet(sheet, sst, out);
    }
    mz_zip_reader_end(&z);
    return out;
}

template <typename F>
static double best_of(int reps, F f, Cells& keep) {
    double best = 1e18;
    for (int r = 0; r < reps; r++) {
        auto t0 = clk::now(); Cells c = f(); auto t1 = clk::now();
        best = std::min(best, ms(t0, t1));
        keep = std::move(c);
    }
    return best;
}

// "hand the gnarly bits to xlnt": open() parses styles/SST/merges up front, defers cells.
// Time open() WITHOUT reading a single cell — the cost of the workbook-level metadata only.
static double open_only(const std::vector<uint8_t>& bytes) {
    double best = 1e18;
    for (int r = 0; r < 3; r++) {
        auto t0 = clk::now();
        { xlnt::streaming_workbook_reader rr; rr.open(bytes); auto t = rr.sheet_titles(); (void)t; }
        auto t1 = clk::now();
        best = std::min(best, ms(t0, t1));
    }
    return best;
}

int main(int argc, char** argv) {
    std::printf("%-26s %9s %9s %9s %9s %8s %8s\n",
                "file", "load", "streamR", "openOnly", "scan", "cells", "scan/str");
    for (int i = 1; i < argc; i++) {
        auto bytes = read_file(argv[i]);
        if (bytes.empty()) { std::printf("%-26s (unreadable)\n", argv[i]); continue; }
        Cells cl, cr, cs;
        double L = best_of(3, [&]{ return build_load(bytes); },        cl);
        double R = best_of(3, [&]{ return build_stream_xlnt(bytes); }, cr);
        double O = open_only(bytes);
        double S = best_of(3, [&]{ return build_scan(bytes); },        cs);
        const char* base = std::strrchr(argv[i], '/');
        std::printf("%-26s %9.1f %9.1f %9.1f %9.1f %8zu %7.1fx\n",
                    base ? base + 1 : argv[i], L, R, O, S, cs.size(), R / S);
    }
    return 0;
}
