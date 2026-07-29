/* Legacy .xls (BIFF) WALKER lane — the things libxls does NOT expose: cell FORMULAS, defined NAMES, VBA.
 *
 * STRICT lanes: this file is libxls-free. libxls (bboxes_xls.cpp) owns values / number-formats / sheet list;
 * this file owns formulas / names / VBA, read straight from the BIFF record stream inside the OLE2/CFB
 * container. Decoding is written from the Microsoft open specs [MS-XLS] / [MS-OVBA] — no GPL/LGPL/MPL code.
 * CFB access is microsoft/compoundfilereader (MIT, vendored in third_party/).
 *
 * Surfaced the bboxes way (NOT a bespoke JSON model): defined names -> xls_names(); cell formulas will
 * populate the bbox `formula` field; VBA -> xls_vba(). This commit implements the defined-name INVENTORY
 * (name text + scope + flags); the name's TARGET formula and cell formulas wait on the Ptg parser.
 */
#include "bboxes.h"

#include "compoundfilereader.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {
using json = nlohmann::json;

std::vector<char> slurp(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamsize n = f.tellg(); f.seekg(0);
    std::vector<char> b(n > 0 ? static_cast<size_t>(n) : 0);
    if (n > 0 && !f.read(b.data(), n)) b.clear();
    return b;
}

std::string cfb_entry_name(const CFB::COMPOUND_FILE_ENTRY* e) {   /* UTF-16 name -> ASCII (our targets are ASCII) */
    std::string s; int chars = e->nameLen >= 2 ? e->nameLen / 2 - 1 : 0;
    for (int i = 0; i < chars && i < 31; i++) { uint16_t ch = e->name[i]; s += (ch && ch < 128) ? char(ch) : '?'; }
    return s;
}

/* [MS-XLS] 2.5.97 built-in name ids (fBuiltin names carry a 1-byte id instead of text). */
const char* builtin_name(uint8_t id) {
    switch (id) {
        case 0x00: return "Consolidate_Area"; case 0x01: return "Auto_Open";  case 0x02: return "Auto_Close";
        case 0x03: return "Extract";          case 0x04: return "Database";   case 0x05: return "Criteria";
        case 0x06: return "Print_Area";       case 0x07: return "Print_Titles"; case 0x08: return "Recorder";
        case 0x09: return "Data_Form";        case 0x0A: return "Auto_Activate"; case 0x0B: return "Auto_Deactivate";
        case 0x0C: return "Sheet_Title";      case 0x0D: return "_FilterDatabase";
        default:   return nullptr;
    }
}

/* Read the workbook BIFF stream out of the OLE2/CFB container ("Workbook", or "Book" for BIFF5/7). */
bool read_workbook_stream(const void* buf, size_t len, std::vector<char>& out, std::string& err) {
    try {
        CFB::CompoundFileReader cfb(buf, len);
        const CFB::COMPOUND_FILE_ENTRY* wb = nullptr;
        cfb.EnumFiles(cfb.GetRootEntry(), -1, [&](const CFB::COMPOUND_FILE_ENTRY* e, const std::u16string&, int) {
            if (cfb.IsStream(e)) { std::string nm = cfb_entry_name(e); if (nm == "Workbook" || nm == "Book") wb = e; }
        });
        if (!wb) { err = "no Workbook/Book stream (not a legacy .xls?)"; return false; }
        out.resize(static_cast<size_t>(wb->size));
        cfb.ReadFile(wb, 0, out.data(), out.size());
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

}  // namespace

/* ── xls_names() — defined-name inventory (mirrors the xls_metadata/xls_style_decode side-channels) ──── */

const char* bboxes_xls_names_json(const void* buf, size_t len) {
    static thread_local std::string out;
    json o; o["dialect"] = "xls";
    std::vector<char> wbuf; std::string err;
    if (!read_workbook_stream(buf, len, wbuf, err)) {
        o["names"] = json::array(); o["error"] = err;
        out = o.dump(-1, ' ', false, json::error_handler_t::replace); return out.c_str();
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(wbuf.data());
    size_t n = wbuf.size(), off = 0;
    json names = json::array();
    bool filepass = false;
    while (off + 4 <= n) {
        uint16_t type = uint16_t(p[off]) | (uint16_t(p[off + 1]) << 8);
        uint16_t rlen = uint16_t(p[off + 2]) | (uint16_t(p[off + 3]) << 8);
        off += 4;
        if (off + rlen > n) break;
        if (type == 0x002F) { filepass = true; break; }   /* FILEPASS: encrypted */
        if (type == 0x0018 && rlen >= 15) {                /* Lbl / NAME  [MS-XLS] 2.4.149 */
            const uint8_t* r = p + off;
            uint16_t grbit = uint16_t(r[0]) | (uint16_t(r[1]) << 8);
            uint8_t  cch   = r[3];
            uint16_t cce   = uint16_t(r[4]) | (uint16_t(r[5]) << 8);
            uint16_t itab  = uint16_t(r[8]) | (uint16_t(r[9]) << 8);
            bool hidden  = (grbit & 0x0001) != 0;
            bool builtin = (grbit & 0x0020) != 0;
            /* name = XLUnicodeStringNoCch at r[14]: 1 flags byte (fHighByte), then cch code units */
            std::string name;
            if (rlen >= 15u) {
                uint8_t flags = r[14]; const uint8_t* s = r + 15;
                if (builtin && cch >= 1) {
                    const char* bn = builtin_name(s[0]);
                    name = bn ? bn : ("builtin#" + std::to_string(s[0]));
                } else if (flags & 0x01) {                 /* UTF-16LE */
                    for (uint32_t i = 0; i < cch && (15u + 2u * i + 1u) < rlen; i++) {
                        uint16_t ch = uint16_t(s[2 * i]) | (uint16_t(s[2 * i + 1]) << 8);
                        name += (ch < 128) ? char(ch) : '?';
                    }
                } else {                                    /* 8-bit compressed */
                    for (uint32_t i = 0; i < cch && (15u + i) < rlen; i++) name += char(s[i]);
                }
            }
            names.push_back({
                {"name", name},
                {"scope", itab == 0 ? json(nullptr) : json((int)itab - 1)},   /* null = workbook; else 0-based sheet */
                {"hidden", hidden}, {"builtin", builtin},
                {"has_target_formula", cce > 0}   /* target Ptg present; rendered once the Ptg parser lands */
            });
        }
        off += rlen;
    }
    if (filepass) { o["names"] = json::array(); o["error"] = "FILEPASS: workbook is encrypted (out of scope)"; }
    else          { o["names"] = std::move(names); }
    out = o.dump(-1, ' ', false, json::error_handler_t::replace);
    return out.c_str();
}
const char* bboxes_xls_names_json_file(const char* path) {
    std::vector<char> s = slurp(path);
    return bboxes_xls_names_json(s.data(), s.size());
}
