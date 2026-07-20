// Dialect-specific document metadata → one JSON clob (full-take, single pass).
// Low-level XML transformation: miniz (unzip) + pugixml (DOM walk) → nlohmann::json.
// Stream-oriented: the *_file entry points use mz_zip_reader_init_file, which seeks
// and decompresses ONLY the small metadata parts — never the whole workbook body.
// Recognition (what dialect a blob is) is a separate concern; these assume the dialect.
#include "bboxes.h"
#include <miniz.h>
#include <pugixml.hpp>
#include <nlohmann/json.hpp>
#include "sha1.h"
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cctype>
#include <cstdlib>

using json = nlohmann::json;

// Per-thread storage backing the returned const char* (valid until the next call).
static thread_local std::string g_result;

// ── pugixml helpers: descendant search by namespace-stripped LOCAL name ──
// (pugixml keeps the prefix in node.name(); OOXML mixes default- and vt:/r:-prefixed
//  parts, so we compare on the local name.)
namespace {
const char* local(const char* n) { const char* p = std::strchr(n, ':'); return p ? p + 1 : n; }
pugi::xml_node first_by(const pugi::xml_node& n, const char* name) {
    for (auto c : n.children()) {
        if (!std::strcmp(local(c.name()), name)) return c;
        if (auto r = first_by(c, name)) return r;
    }
    return {};
}
void all_by(const pugi::xml_node& n, const char* name, std::vector<pugi::xml_node>& out) {
    for (auto c : n.children()) {
        if (!std::strcmp(local(c.name()), name)) out.push_back(c);
        all_by(c, name, out);
    }
}
std::vector<pugi::xml_node> all_by(const pugi::xml_node& n, const char* name) {
    std::vector<pugi::xml_node> v; all_by(n, name, v); return v;
}
bool truthy(const char* s) { return s && (!std::strcmp(s, "1") || !std::strcmp(s, "true")); }

// Decode OOXML _xNNNN_ escapes (used for control chars that can't appear
// literally, e.g. _x000d_ = CR, _x0009_ = tab) to UTF-8. Lossless but not
// round-trippable — matches the metadata-cleanup convention.
std::string unescape_ooxml(const char* cstr) {
    std::string s = cstr ? cstr : "";
    if (s.find("_x") == std::string::npos) return s;
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (i + 7 <= s.size() && s[i] == '_' && s[i + 1] == 'x' && s[i + 6] == '_' &&
            std::isxdigit((unsigned char)s[i + 2]) && std::isxdigit((unsigned char)s[i + 3]) &&
            std::isxdigit((unsigned char)s[i + 4]) && std::isxdigit((unsigned char)s[i + 5])) {
            unsigned cp = (unsigned)std::strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
            if (cp < 0x80) {
                out += (char)cp;
            } else if (cp < 0x800) {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
            } else {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
            i += 7;
        } else {
            out += s[i++];
        }
    }
    return out;
}

std::vector<std::string> zip_list(mz_zip_archive& z) {
    std::vector<std::string> out;
    mz_uint n = mz_zip_reader_get_num_files(&z);
    for (mz_uint i = 0; i < n; i++) {
        char name[512];
        if (mz_zip_reader_get_filename(&z, i, name, sizeof(name))) out.emplace_back(name);
    }
    return out;
}
bool zip_load(mz_zip_archive& z, const char* part, pugi::xml_document& doc) {
    int idx = mz_zip_reader_locate_file(&z, part, nullptr, 0);
    if (idx < 0) return false;
    size_t sz = 0;
    void* p = mz_zip_reader_extract_to_heap(&z, idx, &sz, 0);
    if (!p) return false;
    bool ok = doc.load_buffer(p, sz);
    mz_free(p);
    return ok;
}
bool zip_bytes(mz_zip_archive& z, const char* part, std::string& out) {
    int idx = mz_zip_reader_locate_file(&z, part, nullptr, 0);
    if (idx < 0) return false;
    size_t sz = 0;
    void* p = mz_zip_reader_extract_to_heap(&z, idx, &sz, 0);
    if (!p) return false;
    out.assign(static_cast<char*>(p), sz);
    mz_free(p);
    return true;
}
std::string base64(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];  out += T[v & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t v = (uint8_t)in[i] << 16;
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8);
        out += T[(v >> 18) & 63]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += "=";
    }
    return out;
}

// ── the actual extraction over an already-opened zip archive ─────────────
std::string xlsx_meta_from_zip(mz_zip_archive& z) {
    json out;
    out["dialect"] = "xlsx";
    out["integrity"] = {{"status", "clean"}};
    auto parts = zip_list(z);
    auto has = [&](const char* p) { for (auto& q : parts) if (q == p) return true; return false; };
    pugi::xml_document doc;

    if (zip_load(z, "docProps/core.xml", doc)) {
        json core = json::object();
        for (auto c : doc.first_child().children())
            if (c.text() && *c.text().get()) core[local(c.name())] = unescape_ooxml(c.text().get());
        if (!core.empty()) out["core"] = core;
    }
    if (zip_load(z, "docProps/app.xml", doc)) {
        json app = json::object();
        for (auto c : doc.first_child().children()) {
            bool has_elem = false;
            for (auto k : c.children()) if (k.type() == pugi::node_element) { has_elem = true; break; }
            if (!has_elem && c.text() && *c.text().get()) app[local(c.name())] = unescape_ooxml(c.text().get());
        }
        if (auto top = first_by(doc, "TitlesOfParts")) {
            json a = json::array();
            for (auto lp : all_by(top, "lpstr")) a.push_back(unescape_ooxml(lp.text().get()));
            app["TitlesOfParts"] = a;
        }
        if (!app.empty()) out["app"] = app;
    }
    if (zip_load(z, "docProps/custom.xml", doc)) {
        json cu = json::object();
        for (auto p : all_by(doc, "property")) {
            const char* nm = p.attribute("name").as_string(nullptr);
            if (nm) cu[nm] = unescape_ooxml(p.first_child().first_child().text().get());
        }
        if (!cu.empty()) out["custom"] = cu;
    }
    if (zip_load(z, "xl/workbook.xml", doc)) {
        json wb = json::object();
        if (auto pr = first_by(doc, "workbookPr")) wb["date1904"] = truthy(pr.attribute("date1904").as_string());
        if (auto calc = first_by(doc, "calcPr")) wb["calc_mode"] = calc.attribute("calcMode").as_string("auto");
        if (auto wv = first_by(doc, "workbookView"); wv && wv.attribute("activeTab"))
            wb["active_tab"] = wv.attribute("activeTab").as_int();
        if (first_by(doc, "workbookProtection")) wb["protected"] = true;
        json sheets = json::array();
        for (auto s : all_by(doc, "sheet"))
            sheets.push_back({{"name", s.attribute("name").as_string()},
                              {"sheet_id", s.attribute("sheetId").as_string()},
                              {"state", s.attribute("state").as_string("visible")}});
        wb["sheet_count"] = sheets.size();
        out["workbook"] = wb;
        out["sheets"] = sheets;
        json names = json::array();
        for (auto n : all_by(doc, "definedName")) {
            const char* lsid = n.attribute("localSheetId").as_string(nullptr);
            names.push_back({{"name", n.attribute("name").as_string()},
                             {"refers_to", n.text().get()},
                             {"scope", lsid ? std::string("sheet:") + lsid : std::string("workbook")},
                             {"hidden", truthy(n.attribute("hidden").as_string())}});
        }
        if (!names.empty()) out["names"] = names;
    }
    // per worksheet part: codeName / tabColor / dimension / protection / autofilter
    json wsparts = json::object();
    for (auto& part : parts) {
        if (part.rfind("xl/worksheets/sheet", 0) != 0 || part.size() < 5 ||
            part.substr(part.size() - 4) != ".xml") continue;
        if (!zip_load(z, part.c_str(), doc)) continue;
        json info = json::object();
        if (auto spr = first_by(doc, "sheetPr")) {
            if (spr.attribute("codeName")) info["code_name"] = spr.attribute("codeName").as_string();
            if (auto tc = first_by(spr, "tabColor")) info["tab_color"] = tc.attribute("rgb").as_string();
        }
        if (auto dim = first_by(doc, "dimension")) info["dimension"] = dim.attribute("ref").as_string();
        if (first_by(doc, "sheetProtection")) info["protected"] = true;
        if (auto af = first_by(doc, "autoFilter")) info["autofilter"] = af.attribute("ref").as_string();
        if (!info.empty()) wsparts[part] = info;
    }
    if (!wsparts.empty()) out["_worksheet_parts"] = wsparts;
    // external links: structure only, cached VALUES never read
    json ext = json::array();
    for (auto& part : parts) {
        if (part.rfind("xl/externalLinks/externalLink", 0) != 0 ||
            part.substr(part.size() - 4) != ".xml") continue;
        if (!zip_load(z, part.c_str(), doc)) continue;
        json e; e["part"] = part;
        if (auto sn = first_by(doc, "sheetNames")) {
            json a = json::array();
            for (auto s : all_by(sn, "sheetName")) a.push_back(s.attribute("val").as_string());
            e["sheets"] = a;
        }
        if (auto dn = first_by(doc, "definedNames")) {
            json a = json::array();
            for (auto d : all_by(dn, "definedName")) a.push_back(d.attribute("name").as_string());
            e["names"] = a;
        }
        json cells = json::array();
        for (auto c : all_by(doc, "cell")) if (c.attribute("r")) cells.push_back(c.attribute("r").as_string());
        if (!cells.empty()) e["cells"] = cells;
        std::string rp = "xl/externalLinks/_rels/" + part.substr(part.rfind('/') + 1) + ".rels";
        pugi::xml_document rdoc;
        if (zip_load(z, rp.c_str(), rdoc))
            for (auto rel : all_by(rdoc, "Relationship"))
                if (rel.attribute("Target")) { e["target"] = rel.attribute("Target").as_string(); break; }
        ext.push_back(e);
    }
    if (!ext.empty()) out["external_links"] = ext;
    json tabs = json::array();
    for (auto& part : parts) {
        if (part.rfind("xl/tables/table", 0) != 0 || part.substr(part.size() - 4) != ".xml") continue;
        if (!zip_load(z, part.c_str(), doc)) continue;
        json cols = json::array();
        for (auto c : all_by(doc, "tableColumn")) cols.push_back(c.attribute("name").as_string());
        tabs.push_back({{"name", doc.first_child().attribute("name").as_string()},
                        {"display_name", doc.first_child().attribute("displayName").as_string()},
                        {"ref", doc.first_child().attribute("ref").as_string()},
                        {"columns", cols}});
    }
    if (!tabs.empty()) out["tables"] = tabs;
    // VBA: surface identity only (present / size / SHA-1). The vbaProject.bin
    // is an OLE2/CFB container — its parsing (modules, references, source) is
    // deliberately left to a dedicated downstream library (olefile/oletools);
    // xlsx_vba_base64() hands out the raw bytes for that. The SHA-1 lets you
    // JOIN/cluster identical VBA projects across a corpus without shipping bytes.
    if (has("xl/vbaProject.bin")) {
        std::string bin;
        if (zip_bytes(z, "xl/vbaProject.bin", bin)) {
            SHA1 sha1;
            out["vba"] = {{"present", true},
                          {"size", static_cast<uint64_t>(bin.size())},
                          {"sha1", sha1(bin.data(), bin.size())}};
        } else {
            out["vba"] = {{"present", true}};
        }
    } else {
        out["vba"] = {{"present", false}};
    }
    return out.dump(1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// ── manifest: central-directory-only view (the "last N bytes" tier) ──────
// The zip EOCD + central directory live at the file tail; enumerating them
// yields every part's name without reading a single part body. From that
// alone we derive structural facts (has VBA? worksheet count? external links?).
std::string xlsx_manifest_from_zip(mz_zip_archive& z) {
    auto parts = zip_list(z);
    auto has = [&](const char* p) { for (auto& q : parts) if (q == p) return true; return false; };
    auto count = [&](const char* pre, const char* suf) {
        int n = 0; size_t pl = std::strlen(pre), sl = std::strlen(suf);
        for (auto& q : parts)
            if (q.size() >= pl + sl && q.compare(0, pl, pre) == 0 &&
                q.compare(q.size() - sl, sl, suf) == 0) n++;
        return n;
    };
    json out;
    out["dialect"] = "xlsx";
    out["part_count"] = static_cast<int>(parts.size());
    out["worksheet_parts"]   = count("xl/worksheets/sheet", ".xml");
    out["chartsheet_parts"]  = count("xl/chartsheets/sheet", ".xml");
    out["table_parts"]       = count("xl/tables/table", ".xml");
    out["pivot_parts"]       = count("xl/pivotTables/pivotTable", ".xml");
    out["has_vba"]           = has("xl/vbaProject.bin");
    out["has_external_links"] = count("xl/externalLinks/externalLink", ".xml") > 0;
    out["has_connections"]   = has("xl/connections.xml");
    out["has_xml_maps"]      = has("xl/xmlMaps.xml");
    out["has_custom_props"]  = has("docProps/custom.xml");
    json list = json::array();
    for (auto& q : parts) list.push_back(q);
    out["parts"] = list;
    return out.dump(1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// ── recursive container walk ─────────────────────────────────────────────
// A blob is a tree of typed blobs: a plain zip holds files (some themselves
// zips/PDFs), an OOXML package is a dialect leaf. Sniff each node by magic
// bytes, dispatch to the matching extractor, recurse into nested containers.
constexpr int kMaxDepth = 8;

enum Kind { K_ZIP, K_PDF, K_OLE, K_XML, K_OTHER };
Kind sniff(const unsigned char* b, size_t n) {
    if (n >= 4 && b[0] == 0x25 && b[1] == 0x50 && b[2] == 0x44 && b[3] == 0x46) return K_PDF; // %PDF
    if (n >= 4 && b[0] == 'P' && b[1] == 'K' && (b[2] == 3 || b[2] == 5 || b[2] == 7)) return K_ZIP;
    if (n >= 8 && b[0] == 0xD0 && b[1] == 0xCF && b[2] == 0x11 && b[3] == 0xE0) return K_OLE;
    if (n >= 5 && b[0] == '<' && b[1] == '?' && b[2] == 'x' && b[3] == 'm' && b[4] == 'l') return K_XML;
    return K_OTHER;
}

json walk_bytes(const void* data, size_t len, const std::string& name, int depth);

json walk_zip(const void* data, size_t len, const std::string& name, int depth) {
    json node; node["name"] = name;
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, data, len, 0)) {
        node["dialect"] = "zip"; node["error"] = "unreadable zip"; return node;
    }
    auto parts = zip_list(z);
    auto has = [&](const char* p) { for (auto& q : parts) if (q == p) return true; return false; };
    if (has("[Content_Types].xml")) {                 // an OOXML package → dialect leaf
        if (has("xl/workbook.xml")) {
            node["dialect"] = "xlsx";
            node["metadata"] = json::parse(xlsx_meta_from_zip(z));
        } else if (has("word/document.xml")) {
            node["dialect"] = "docx";
        } else if (has("ppt/presentation.xml")) {
            node["dialect"] = "pptx";
        } else {
            node["dialect"] = "ooxml";
        }
        mz_zip_reader_end(&z);
        return node;
    }
    node["dialect"] = "zip";                           // plain archive → recurse members
    node["member_count"] = static_cast<int>(parts.size());
    if (depth >= kMaxDepth) {
        node["truncated"] = "max depth";
        mz_zip_reader_end(&z);
        return node;
    }
    json kids = json::array();
    for (auto& part : parts) {
        if (!part.empty() && part.back() == '/') continue;   // directory entry
        std::string mb;
        if (!zip_bytes(z, part.c_str(), mb)) continue;
        kids.push_back(walk_bytes(mb.data(), mb.size(), part, depth + 1));
    }
    node["children"] = kids;
    mz_zip_reader_end(&z);
    return node;
}

json walk_bytes(const void* data, size_t len, const std::string& name, int depth) {
    const unsigned char* b = static_cast<const unsigned char*>(data);
    switch (sniff(b, len)) {
        case K_ZIP: return walk_zip(data, len, name, depth);
        case K_PDF: {
            json n; n["name"] = name; n["dialect"] = "pdf";
            n["metadata"] = json::parse(bboxes_pdf_metadata_json(data, len));
            return n;
        }
        case K_OLE: {   // legacy .xls/.doc or a bare vbaProject.bin — leaf for now
            json n; n["name"] = name; n["dialect"] = "ole";
            n["size"] = static_cast<uint64_t>(len); return n;
        }
        case K_XML: {
            json n; n["name"] = name; n["dialect"] = "xml";
            n["size"] = static_cast<uint64_t>(len); return n;
        }
        default: {
            json n; n["name"] = name; n["dialect"] = "binary";
            n["size"] = static_cast<uint64_t>(len); return n;
        }
    }
}

// read a whole file into a string (walk must recurse; nested members need inflating)
bool slurp(const char* path, std::string& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) { std::fclose(f); return false; }
    out.resize(static_cast<size_t>(sz));
    size_t got = std::fread(&out[0], 1, out.size(), f);
    std::fclose(f);
    out.resize(got);
    return true;
}
} // namespace

extern "C" {

// Stream-oriented: reads only the metadata parts off the file, not the whole body.
const char* bboxes_xlsx_metadata_json_file(const char* path) {
    mz_zip_archive z{};
    if (!mz_zip_reader_init_file(&z, path, 0)) {
        g_result = "{\"dialect\":\"xlsx\",\"integrity\":{\"status\":\"failed\",\"error\":\"not a zip / unreadable\"}}";
        return g_result.c_str();
    }
    g_result = xlsx_meta_from_zip(z);
    mz_zip_reader_end(&z);
    return g_result.c_str();
}

// Buffer variant (when the bytes are already in hand, e.g. a DB BLOB).
const char* bboxes_xlsx_metadata_json(const void* data, size_t len) {
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, data, len, 0)) {
        g_result = "{\"dialect\":\"xlsx\",\"integrity\":{\"status\":\"failed\",\"error\":\"not a zip\"}}";
        return g_result.c_str();
    }
    g_result = xlsx_meta_from_zip(z);
    mz_zip_reader_end(&z);
    return g_result.c_str();
}

// Manifest (tail-only tier): central directory → part list + structural facts.
const char* bboxes_xlsx_manifest_json_file(const char* path) {
    mz_zip_archive z{};
    if (!mz_zip_reader_init_file(&z, path, 0)) {
        g_result = "{\"dialect\":\"xlsx\",\"integrity\":{\"status\":\"failed\",\"error\":\"not a zip / unreadable\"}}";
        return g_result.c_str();
    }
    g_result = xlsx_manifest_from_zip(z);
    mz_zip_reader_end(&z);
    return g_result.c_str();
}
const char* bboxes_xlsx_manifest_json(const void* data, size_t len) {
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, data, len, 0)) {
        g_result = "{\"dialect\":\"xlsx\",\"integrity\":{\"status\":\"failed\",\"error\":\"not a zip\"}}";
        return g_result.c_str();
    }
    g_result = xlsx_manifest_from_zip(z);
    mz_zip_reader_end(&z);
    return g_result.c_str();
}

// Raw vbaProject.bin as base64 (empty string when absent) — the "get it out"
// hook so a downstream OLE library parses modules/references/source.
const char* bboxes_xlsx_vba_base64_file(const char* path) {
    mz_zip_archive z{};
    if (!mz_zip_reader_init_file(&z, path, 0)) { g_result.clear(); return g_result.c_str(); }
    std::string bin;
    bool got = zip_bytes(z, "xl/vbaProject.bin", bin);
    mz_zip_reader_end(&z);
    g_result = got ? base64(bin) : std::string();
    return g_result.c_str();
}
const char* bboxes_xlsx_vba_base64(const void* data, size_t len) {
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, data, len, 0)) { g_result.clear(); return g_result.c_str(); }
    std::string bin;
    bool got = zip_bytes(z, "xl/vbaProject.bin", bin);
    mz_zip_reader_end(&z);
    g_result = got ? base64(bin) : std::string();
    return g_result.c_str();
}

// Recursive container walk → tree of typed nodes.
const char* bboxes_container_walk_json(const void* data, size_t len) {
    g_result = walk_bytes(data, len, "<root>", 0).dump(1, ' ', false, nlohmann::json::error_handler_t::replace);
    return g_result.c_str();
}
const char* bboxes_container_walk_json_file(const char* path) {
    std::string buf;
    if (!slurp(path, buf)) {
        g_result = "{\"error\":\"file not found / unreadable\"}";
        return g_result.c_str();
    }
    std::string base = path;
    auto p = base.rfind('/');
    if (p != std::string::npos) base = base.substr(p + 1);
    g_result = walk_bytes(buf.data(), buf.size(), base, 0).dump(1, ' ', false, nlohmann::json::error_handler_t::replace);
    return g_result.c_str();
}

} // extern "C"
