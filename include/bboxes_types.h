#ifndef BBOXES_TYPES_H
#define BBOXES_TYPES_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

/* ── Shared constants ─────────────────────────────────────────── */
constexpr double      BBOXES_DEFAULT_FONT_SIZE = 12.0;
constexpr const char* BBOXES_DEFAULT_COLOR     = "rgba(0,0,0,255)";
constexpr const char* BBOXES_DEFAULT_WEIGHT    = "normal";

inline std::string color_string(unsigned r, unsigned g, unsigned b, unsigned a) {
    char buf[40];
    snprintf(buf, sizeof(buf), "rgba(%u,%u,%u,%u)", r, g, b, a);
    return buf;
}

/* Infer typographic traits from a PostScript / TrueType font name.
   PDF font descriptors often lack weight/width metadata, but the name
   encodes it reliably (e.g. "Helvetica-BoldOblique", "MyriadPro-SemiboldCond").

   Delimiters vary: hyphen, space, comma, or camelCase boundaries.
   We do case-insensitive substring matching which handles all of these. */
struct FontNameTraits {
    bool bold;          /* weight >= 700 */
    bool italic;
    int  weight;        /* CSS-style: 100–950, 0 = unknown */
    bool condensed;     /* any narrow/condensed/compressed variant */
};

inline FontNameTraits font_name_traits(const char* name) {
    FontNameTraits t = {false, false, 0, false};
    if (!name || !*name) return t;

    /* Work on a lowercased copy */
    std::string lc(name);
    for (auto& c : lc) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    /* ── Weight ── (check specific tokens before generic "bold")
       Order matters: "extrabold" must match before "bold".  */
    if (lc.find("thin")       != std::string::npos ||
        lc.find("hairline")   != std::string::npos)   { t.weight = 100; }
    else if (lc.find("ultralight")  != std::string::npos ||
             lc.find("extralight")  != std::string::npos) { t.weight = 200; }
    else if (lc.find("light")       != std::string::npos)  { t.weight = 300; }
    else if (lc.find("medium")      != std::string::npos &&
             lc.find("semi")        == std::string::npos)  { t.weight = 500; }
    else if (lc.find("semibold")    != std::string::npos ||
             lc.find("demibold")    != std::string::npos ||
             lc.find("demi")        != std::string::npos)  { t.weight = 600; }
    else if (lc.find("extrabold")   != std::string::npos ||
             lc.find("ultrabold")   != std::string::npos)  { t.weight = 800; }
    else if (lc.find("black")       != std::string::npos ||
             lc.find("heavy")       != std::string::npos)  { t.weight = 900; }
    else if (lc.find("bold")        != std::string::npos)  { t.weight = 700; }

    t.bold = (t.weight >= 700);

    /* ── Italic / Oblique ── */
    if (lc.find("italic")  != std::string::npos ||
        lc.find("oblique") != std::string::npos ||
        lc.find("slanted") != std::string::npos ||
        /* Short form: "Ital" at end of name, but not as part of "Italic" */
        (lc.size() >= 4 && lc.substr(lc.size() - 4) == "ital"))
    { t.italic = true; }

    /* ── Width ── */
    if (lc.find("condensed")  != std::string::npos ||
        lc.find("cond")       != std::string::npos ||
        lc.find("narrow")     != std::string::npos ||
        lc.find("compressed") != std::string::npos ||
        lc.find("compact")    != std::string::npos)
    { t.condensed = true; }

    return t;
}

/* ── Font table (intern by name only) ──────────────────────────── */

struct FontTable {
    std::unordered_map<std::string, uint32_t> map;
    struct Entry { uint32_t id; std::string name; };
    std::vector<Entry> entries;

    uint32_t intern(const char* name) {
        std::string key = name ? name : "";
        auto it = map.find(key);
        if (it != map.end()) return it->second;
        uint32_t id = static_cast<uint32_t>(entries.size());
        map[key] = id;
        entries.push_back({id, key});
        return id;
    }
};

/* ── Style table (intern by font_id + visual properties) ───────── */

struct StyleKey {
    uint32_t    font_id;
    double      font_size;
    std::string color;
    std::string weight;
    bool        italic;
    bool        underline;

    bool operator==(const StyleKey& o) const {
        return font_id == o.font_id && font_size == o.font_size &&
               color == o.color && weight == o.weight &&
               italic == o.italic && underline == o.underline;
    }
};

struct StyleKeyHash {
    size_t operator()(const StyleKey& k) const {
        size_t h = std::hash<uint32_t>{}(k.font_id);
        h ^= std::hash<double>{}(k.font_size) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(k.color) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(k.weight) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.italic) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<bool>{}(k.underline) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct StyleTable {
    std::unordered_map<StyleKey, uint32_t, StyleKeyHash> map;
    struct Entry {
        uint32_t    id;
        uint32_t    font_id;
        double      font_size;
        std::string color;
        std::string weight;
        bool        italic;
        bool        underline;
    };
    std::vector<Entry> entries;

    uint32_t intern(uint32_t font_id, double font_size,
                    const std::string& color, const std::string& weight,
                    bool italic, bool underline) {
        StyleKey key{font_id, font_size, color, weight, italic, underline};
        auto it = map.find(key);
        if (it != map.end()) return it->second;
        uint32_t id = static_cast<uint32_t>(entries.size());
        map[key] = id;
        entries.push_back({id, font_id, font_size, color, weight, italic, underline});
        return id;
    }
};

/* ── Extraction result (produced by backends) ──────────────────── */

struct BBox {
    uint32_t    page_id;
    uint32_t    style_id;
    double      x, y, w, h;
    std::string text;
    std::string formula;
};

struct Page {
    uint32_t    page_id;
    uint32_t    document_id;
    int         page_number;   /* 1-based */
    double      width, height;
    std::vector<BBox> bboxes;
};

struct BBoxResult {
    std::string source_type;   /* "pdf", "xlsx", ... */
    std::string checksum;      /* MD5 hex of source bytes */
    int         page_count;
    FontTable   fonts;
    StyleTable  styles;
    std::vector<Page> pages;
};

/* ── Backend interface ─────────────────────────────────────────── */

BBoxResult extract_pdf(const void* buf, size_t len, const char* password,
                        int start_page, int end_page);

/* Object-level PDF extraction: one bbox per PDF text object (word/phrase).
   Slower than char-by-char but produces clean text without downstream merging.
   Useful for interactive exploration. */
BBoxResult extract_pdf_objects(const void* buf, size_t len, const char* password,
                                int start_page, int end_page);

BBoxResult extract_xlsx(const void* buf, size_t len, const char* password,
                         int start_page, int end_page);

BBoxResult extract_text(const void* buf, size_t len);

BBoxResult extract_docx(const void* buf, size_t len);

#endif
