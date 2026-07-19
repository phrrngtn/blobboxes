// Dialect-specific document metadata → one JSON clob (full-take, single pass).
// Low-level XML transformation: miniz (unzip) + pugixml (DOM walk) → nlohmann::json.
// Stream-oriented: the *_file entry points use mz_zip_reader_init_file, which seeks
// and decompresses ONLY the small metadata parts — never the whole workbook body.
// Recognition (what dialect a blob is) is a separate concern; these assume the dialect.
#include "bboxes.h"
#include <miniz.h>
#include <pugixml.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <cstring>

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
            if (c.text() && *c.text().get()) core[local(c.name())] = c.text().get();
        if (!core.empty()) out["core"] = core;
    }
    if (zip_load(z, "docProps/app.xml", doc)) {
        json app = json::object();
        for (auto c : doc.first_child().children()) {
            bool has_elem = false;
            for (auto k : c.children()) if (k.type() == pugi::node_element) { has_elem = true; break; }
            if (!has_elem && c.text() && *c.text().get()) app[local(c.name())] = c.text().get();
        }
        if (auto top = first_by(doc, "TitlesOfParts")) {
            json a = json::array();
            for (auto lp : all_by(top, "lpstr")) a.push_back(lp.text().get());
            app["TitlesOfParts"] = a;
        }
        if (!app.empty()) out["app"] = app;
    }
    if (zip_load(z, "docProps/custom.xml", doc)) {
        json cu = json::object();
        for (auto p : all_by(doc, "property")) {
            const char* nm = p.attribute("name").as_string(nullptr);
            if (nm) cu[nm] = p.first_child().first_child().text().get();
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
    out["vba"] = {{"present", has("xl/vbaProject.bin")}};
    return out.dump(1);
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

} // extern "C"
