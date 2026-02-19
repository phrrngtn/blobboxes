#ifndef BBOXES_TYPES_H
#define BBOXES_TYPES_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

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

BBoxResult extract_xlsx(const void* buf, size_t len, const char* password,
                         int start_page, int end_page);

BBoxResult extract_text(const void* buf, size_t len);

BBoxResult extract_docx(const void* buf, size_t len);

#endif
