/* Legacy .xls (BIFF) WALKER lane — formulas / defined names / VBA (the things libxls does NOT expose).
 *
 * STRICT lanes: libxls-free. bboxes_xls.cpp owns values/formats/sheets; this owns the BIFF record stream
 * inside the OLE2/CFB container. Decoding from [MS-XLS] (no GPL/LGPL/MPL). CFB = microsoft/compoundfilereader
 * (MIT, third_party/). Surfaced the bboxes way: xls_names (defined-name inventory + target formula),
 * xls_formulas (every cell formula in R1C1 + A1 with its address). Ptg parser per [MS-XLS] 2.5.198.
 */
#include "bboxes.h"

#include "compoundfilereader.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
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
std::string cfb_entry_name(const CFB::COMPOUND_FILE_ENTRY* e) {
    std::string s; int chars = e->nameLen >= 2 ? e->nameLen / 2 - 1 : 0;
    for (int i = 0; i < chars && i < 31; i++) { uint16_t ch = e->name[i]; s += (ch && ch < 128) ? char(ch) : '?'; }
    return s;
}
const char* builtin_name(uint8_t id) {
    switch (id) {
        case 0x00: return "Consolidate_Area"; case 0x01: return "Auto_Open";  case 0x02: return "Auto_Close";
        case 0x03: return "Extract";          case 0x04: return "Database";   case 0x05: return "Criteria";
        case 0x06: return "Print_Area";       case 0x07: return "Print_Titles"; case 0x08: return "Recorder";
        case 0x09: return "Data_Form";        case 0x0A: return "Auto_Activate"; case 0x0B: return "Auto_Deactivate";
        case 0x0C: return "Sheet_Title";      case 0x0D: return "_FilterDatabase"; default: return nullptr;
    }
}
bool read_workbook_stream(const void* buf, size_t len, std::vector<char>& out, std::string& err) {
    try {
        CFB::CompoundFileReader cfb(buf, len);
        const CFB::COMPOUND_FILE_ENTRY* wb = nullptr;
        cfb.EnumFiles(cfb.GetRootEntry(), -1, [&](const CFB::COMPOUND_FILE_ENTRY* e, const std::u16string&, int) {
            if (cfb.IsStream(e)) { std::string nm = cfb_entry_name(e); if (nm == "Workbook" || nm == "Book") wb = e; }
        });
        if (!wb) { err = "no Workbook/Book stream"; return false; }
        out.resize(static_cast<size_t>(wb->size));
        cfb.ReadFile(wb, 0, out.data(), out.size());
        return true;
    } catch (const std::exception& e) { err = e.what(); return false; }
}

/* ── globals pre-scan: sheet names (BoundSheet8) + ExternSheet XTIs (for 3D ref resolution) ──────────── */
struct SupBookInfo { bool self = false; int ext_index = 0; std::vector<std::string> sheets; };  /* self = this workbook; else external, sheets named here */
struct XTI { int isup = 0, first = 0, last = 0; };                                               /* ExternSheet entry: SupBook + sheet range */
struct Globals { std::vector<std::string> sheets; std::vector<uint32_t> sheet_pos; std::vector<uint8_t> sheet_dt;
                 std::vector<SupBookInfo> supbooks; std::vector<XTI> xtis;
                 std::vector<std::string> lbl_names; uint16_t biff_version = 0x0600; };
Globals scan_globals(const uint8_t* p, size_t n) {
    Globals g; size_t off = 0; bool first_bof = true;
    while (off + 4 <= n) {
        uint16_t type = uint16_t(p[off]) | (uint16_t(p[off+1]) << 8);
        uint16_t len  = uint16_t(p[off+2]) | (uint16_t(p[off+3]) << 8);
        off += 4; if (off + len > n) break;
        const uint8_t* r = p + off;
        if (type == 0x0809 && first_bof) {                   /* first BOF = workbook globals: BIFF version */
            if (len >= 2) g.biff_version = uint16_t(r[0]) | (uint16_t(r[1]) << 8);
            first_bof = false;
        } else if (type == 0x0085 && len >= 8) {             /* BoundSheet8: lbPlyPos(4) hsState(1) dt(1) name@6 */
            uint32_t pos = uint32_t(r[0]) | (uint32_t(r[1])<<8) | (uint32_t(r[2])<<16) | (uint32_t(r[3])<<24);
            uint8_t dt = r[5], cch = r[6], flags = r[7]; std::string nm;
            for (uint32_t i = 0; i < cch; i++) {
                if (flags & 0x01) { size_t o = 8 + 2*i; if (o+1 < len) nm += char(uint16_t(r[o]) | (uint16_t(r[o+1])<<8)); }
                else              { size_t o = 8 + i;   if (o   < len) nm += char(r[o]); }
            }
            g.sheets.push_back(nm); g.sheet_pos.push_back(pos); g.sheet_dt.push_back(dt);
        } else if (type == 0x01AE && len >= 4) {             /* SupBook: ctab, cch(0x0401=self,0x3A01=addin,else external path+sheet names) */
            uint16_t ctab = uint16_t(r[0]) | (uint16_t(r[1]) << 8);
            uint16_t cch  = uint16_t(r[2]) | (uint16_t(r[3]) << 8);
            SupBookInfo sb;
            if (cch == 0x0401 || cch == 0x3A01) { sb.self = true; }
            else {
                sb.self = false;
                size_t pos = 4;
                uint8_t nflags = pos < len ? r[pos] : 0; pos += 1;
                pos += (nflags & 1) ? 2u * cch : cch;         /* skip encoded virtual-path workbook name */
                for (uint16_t t = 0; t < ctab && pos + 3 <= len; t++) {   /* ctab sheet names (XLUnicodeString) */
                    uint16_t scch = uint16_t(r[pos]) | (uint16_t(r[pos+1]) << 8); uint8_t sflags = r[pos+2]; pos += 3;
                    std::string s;
                    for (uint16_t k = 0; k < scch; k++) {
                        if (sflags & 1) { if (pos + 2*k + 1 < len) s += char(uint16_t(r[pos+2*k]) | (uint16_t(r[pos+2*k+1]) << 8)); }
                        else            { if (pos + k < len) s += char(r[pos+k]); }
                    }
                    pos += (sflags & 1) ? 2u * scch : scch;
                    sb.sheets.push_back(s);
                }
            }
            int extc = 0; for (auto& b : g.supbooks) if (!b.self) extc++;
            if (!sb.self) sb.ext_index = extc + 1;            /* 1-based index among external workbooks */
            g.supbooks.push_back(std::move(sb));
        } else if (type == 0x0017 && len >= 2) {             /* ExternSheet: cXTI then {iSupBook,itabFirst,itabLast} */
            uint16_t cxti = uint16_t(r[0]) | (uint16_t(r[1]) << 8);
            for (uint16_t i = 0; i < cxti && 2 + i*6 + 5 < len; i++) {
                XTI x; x.isup  = int16_t(uint16_t(r[2+i*6])   | (uint16_t(r[2+i*6+1]) << 8));
                       x.first = int16_t(uint16_t(r[2+i*6+2]) | (uint16_t(r[2+i*6+3]) << 8));
                       x.last  = int16_t(uint16_t(r[2+i*6+4]) | (uint16_t(r[2+i*6+5]) << 8));
                g.xtis.push_back(x);
            }
        } else if (type == 0x0018 && len >= 15) {            /* Lbl/NAME: capture name (1-based) for PtgName */
            uint16_t grbit = uint16_t(r[0]) | (uint16_t(r[1]) << 8);
            uint8_t cch = r[3]; bool builtin = (grbit & 0x0020);
            uint8_t flags = r[14]; const uint8_t* s = r + 15; std::string nm;
            if (builtin && cch >= 1) { const char* bn = builtin_name(s[0]); nm = bn ? bn : ("builtin#" + std::to_string(s[0])); }
            else if (flags & 0x01) { for (uint32_t k = 0; k < cch && 15u + 2u*k + 1u < len; k++) { uint16_t ch = uint16_t(s[2*k]) | (uint16_t(s[2*k+1]) << 8); nm += (ch < 128) ? char(ch) : '?'; } }
            else { for (uint32_t k = 0; k < cch && 15u + k < len; k++) nm += char(s[k]); }
            g.lbl_names.push_back(nm);
        }
        off += len;
    }
    return g;
}

std::string col_letters(int col) {                            /* 0-based col -> A, B, ... AA */
    std::string s; col += 1;
    while (col > 0) { int r = (col - 1) % 26; s = char('A' + r) + s; col = (col - 1) / 26; }
    return s;
}

/* ── the Ptg parser: rgce token array -> {R1C1, A1} via a dual-notation RPN stack ─────────────────────── */
struct Expr { std::string r1c1, a1; int prec = 99; };
const char* ftab(uint16_t i) {                                /* [MS-XLS] 2.5.198.17 Ftab — common subset */
    switch (i) {
        case 0: return "COUNT"; case 1: return "IF"; case 2: return "ISNA"; case 3: return "ISERROR";
        case 4: return "SUM"; case 5: return "AVERAGE"; case 6: return "MIN"; case 7: return "MAX";
        case 8: return "ROW"; case 9: return "COLUMN"; case 10: return "NA"; case 15: return "SIN";
        case 16: return "COS"; case 19: return "PI"; case 20: return "SQRT"; case 24: return "EXP";
        case 25: return "LN"; case 26: return "LOG10"; case 27: return "ABS"; case 28: return "INT";
        case 29: return "SIGN"; case 30: return "ROUND"; case 31: return "LOOKUP"; case 32: return "INDEX";
        case 36: return "AND"; case 37: return "OR"; case 38: return "NOT"; case 39: return "MOD";
        case 63: return "RAND"; case 74: return "NOW"; case 82: return "SEARCH"; case 97: return "ATAN2";
        case 100: return "CHOOSE"; case 101: return "HLOOKUP"; case 102: return "VLOOKUP"; case 111: return "CHAR";
        case 112: return "LOWER"; case 113: return "UPPER"; case 115: return "LEN"; case 116: return "LEFT";
        case 117: return "RIGHT"; case 118: return "MID"; case 119: return "TEXT"; case 120: return "SUBSTITUTE";
        case 124: return "FIND"; case 148: return "TRIM";
        case 162: return "CLEAN"; case 190: return "ISNUMBER"; case 197: return "TRUNC"; case 212: return "ROUNDUP";
        case 213: return "ROUNDDOWN"; case 216: return "RANK"; case 219: return "ADDRESS"; case 228: return "SUMPRODUCT";
        case 252: return "COUNTIF"; case 269: return "AVERAGEA"; case 336: return "CONCATENATE"; case 345: return "SUMIFS";
        default: return nullptr;
    }
}
/* render one RgceLoc (rw + column-with-flags [MS-XLS] 2.5.198.105/.121).
   mode 0 = ABS  : PtgRef in a Formula — rw/col hold ABSOLUTE addresses; R1C1 rel = value-home.
   mode 1 = N    : PtgRefN / shared-formula base — rel axis holds a SIGNED OFFSET (row int16, col int8),
                   abs axis holds the value; A1 adds the home (member/formula) cell.
   mode 2 = NAME : like N with home=(0,0) — best-effort A1 for relative name refs (names are usually absolute). */
void render_loc2(uint16_t rwf, uint16_t gc, int homeRow, int homeCol, int mode, std::string& r1c1, std::string& a1) {
    int colAbs = gc & 0x3FFF;                     // absolute column (BIFF8 uses low 8 bits, 0..255)
    int rowOff = int(int16_t(rwf));               // signed 16-bit row offset (N/NAME)
    int colOff = int(int8_t(gc & 0xFF));          // signed 8-bit  col offset (N/NAME) — BIFF8 stores it in the low byte
    bool colRel = (gc & 0x4000) != 0, rowRel = (gc & 0x8000) != 0;
    int rowR1, colR1, rowA1, colA1;               // R1C1 relative offsets; A1 absolute 0-based row/col
    if (mode == 0) { rowR1 = int(rwf) - homeRow; colR1 = colAbs - homeCol; rowA1 = int(rwf); colA1 = colAbs; }
    else { int hr = (mode == 1) ? homeRow : 0, hc = (mode == 1) ? homeCol : 0;
           rowR1 = rowOff; colR1 = colOff;
           rowA1 = rowRel ? hr + rowOff : int(rwf); colA1 = colRel ? hc + colOff : colAbs; }
    std::string rp = rowRel ? ("R" + (rowR1 ? "[" + std::to_string(rowR1) + "]" : "")) : ("R" + std::to_string(int(rwf) + 1));
    std::string cp = colRel ? ("C" + (colR1 ? "[" + std::to_string(colR1) + "]" : "")) : ("C" + std::to_string(colAbs + 1));
    r1c1 = rp + cp;
    a1 = (colRel ? "" : "$") + col_letters(colA1) + (rowRel ? "" : "$") + std::to_string(rowA1 + 1);
}
std::string sheet_qual(const Globals& g, uint16_t ixti) {
    if (ixti >= g.xtis.size()) return "";
    const XTI& x = g.xtis[ixti];
    bool external = false; int ext_index = 0; std::string first, last;
    if (x.isup >= 0 && x.isup < (int)g.supbooks.size()) {
        const SupBookInfo& sb = g.supbooks[x.isup];
        const std::vector<std::string>& src = sb.self ? g.sheets : sb.sheets;   /* self -> local sheet names */
        external = !sb.self; ext_index = sb.ext_index;
        if (x.first >= 0 && x.first < (int)src.size()) first = src[x.first];
        if (x.last  >= 0 && x.last  < (int)src.size()) last  = src[x.last];
    }
    std::string prefix = external ? ("[" + std::to_string(ext_index) + "]") : "";
    if (first.empty()) return external ? prefix + "!" : "";
    auto q = [](const std::string& s){ return s.find(' ') != std::string::npos ? "'" + s + "'" : s; };
    std::string sheets = (last.empty() || last == first) ? q(first) : (q(first) + ":" + q(last));   /* Sheet2:Sheet5 */
    return prefix + sheets + "!";
}
std::string ptg_str(const uint8_t* r, size_t avail, size_t& consumed) {   /* ShortXLUnicodeString */
    uint8_t cch = r[0], flags = r[1]; std::string s;
    for (uint32_t i = 0; i < cch; i++) {
        if (flags & 0x01) { size_t o = 2 + 2*i; if (o+1 < avail) s += char(uint16_t(r[o]) | (uint16_t(r[o+1])<<8)); }
        else              { size_t o = 2 + i;   if (o   < avail) s += char(r[o]); }
    }
    consumed = 2 + cch * ((flags & 0x01) ? 2 : 1);
    return s;
}

Expr render_rgce(const uint8_t* rgce, size_t cce, int homeRow, int homeCol, bool hasHome, const Globals& g) {
    std::vector<Expr> st;
    auto push = [&](std::string r1, std::string a1, int prec = 99) { st.push_back({std::move(r1), std::move(a1), prec}); };
    auto binop = [&](const char* sym, int prec) {
        if (st.size() < 2) { push("«?»", "«?»"); return; }
        Expr b = st.back(); st.pop_back(); Expr a = st.back(); st.pop_back();
        auto w = [&](const std::string& s, int p) { return p < prec ? "(" + s + ")" : s; };
        push(w(a.r1c1, a.prec) + sym + w(b.r1c1, b.prec), w(a.a1, a.prec) + sym + w(b.a1, b.prec), prec);
    };
    auto func = [&](const char* name, int argc) {
        std::string r = std::string(name) + "(", a = r; int start = int(st.size()) - argc;
        if (start < 0) start = 0;
        for (int i = start; i < (int)st.size(); i++) { if (i > start) { r += ","; a += ","; } r += st[i].r1c1; a += st[i].a1; }
        st.erase(st.begin() + start, st.end());
        push(r + ")", a + ")");
    };
    auto u16 = [](const uint8_t* q) -> uint16_t { return uint16_t(q[0]) | (uint16_t(q[1]) << 8); };
    size_t i = 0;
    while (i < cce) {
        uint8_t ptg = rgce[i++];
        uint8_t base = ptg >= 0x20 ? (ptg <= 0x3F ? ptg : ptg <= 0x5F ? ptg - 0x20 : ptg - 0x40) : ptg;
        const uint8_t* r = rgce + i; size_t avail = cce - i;
        if (ptg < 0x20) {
            switch (ptg) {
                case 0x03: binop("+", 3); break; case 0x04: binop("-", 3); break;
                case 0x05: binop("*", 4); break; case 0x06: binop("/", 4); break;
                case 0x07: binop("^", 5); break; case 0x08: binop("&", 2); break;
                case 0x09: binop("<", 1); break; case 0x0A: binop("<=",1); break; case 0x0B: binop("=", 1); break;
                case 0x0C: binop(">=",1); break; case 0x0D: binop(">", 1); break; case 0x0E: binop("<>",1); break;
                case 0x0F: binop(" ", 8); break; case 0x10: binop(",", 6); break; case 0x11: binop(":", 8); break;
                case 0x12: if(!st.empty()){auto e=st.back();st.pop_back();push("+"+e.r1c1,"+"+e.a1,6);} break;  /* uplus */
                case 0x13: if(!st.empty()){auto e=st.back();st.pop_back();push("-"+e.r1c1,"-"+e.a1,6);} break;  /* uminus */
                case 0x14: if(!st.empty()){auto e=st.back();st.pop_back();push(e.r1c1+"%",e.a1+"%",7);} break;  /* percent */
                case 0x15: if(!st.empty()){auto e=st.back();st.pop_back();push("("+e.r1c1+")","("+e.a1+")");} break; /* paren */
                case 0x16: push("", ""); break;                                        /* missing arg */
                case 0x17: { size_t used=0; std::string s = avail>=2 ? ptg_str(r,avail,used) : ""; i += used;
                             std::string q="\""+s+"\""; push(q,q); } break;            /* PtgStr */
                case 0x19: { uint8_t grbit = avail ? r[0] : 0;                          /* PtgAttr */
                             if (grbit & 0x04) { uint16_t c = avail >= 3 ? u16(r+1) : 0; i += 3 + (c + 1) * 2; } /* tAttrChoose: skip jump table */
                             else { i += 3; if (grbit & 0x10) func("SUM", 1); } }        /* tAttrSum; space/if/goto/skip are 3 bytes */
                    break;
                case 0x1C: { uint8_t e = avail?r[0]:0; i += 1; const char* s = e==0x17?"#REF!":e==0x07?"#DIV/0!":e==0x0F?"#VALUE!":e==0x1D?"#NAME?":e==0x24?"#N/A":"#ERR!"; push(s,s);} break;
                case 0x1D: { std::string s = (avail && r[0])?"TRUE":"FALSE"; i += 1; push(s,s);} break;   /* bool */
                case 0x1E: { int v = avail>=2 ? (uint16_t(r[0])|(uint16_t(r[1])<<8)) : 0; i += 2; push(std::to_string(v),std::to_string(v)); } break; /* int */
                case 0x1F: { double d=0; if(avail>=8) memcpy(&d,r,8); i += 8; char b[32]; snprintf(b,sizeof b,"%.15g",d); push(b,b);} break; /* num */
                default: i = cce; break;                                               /* unknown control -> stop */
            }
        } else {
            switch (base) {
                case 0x24: { int m=hasHome?0:2; if (avail>=4){ std::string a,b; render_loc2(u16(r),u16(r+2),homeRow,homeCol,m,a,b); push(a,b);} i += 4; } break;   /* PtgRef */
                case 0x2C: { if (avail>=4){ std::string a,b; render_loc2(u16(r),u16(r+2),homeRow,homeCol,1,a,b);  push(a,b);} i += 4; } break;                        /* PtgRefN (offset) */
                case 0x25: case 0x2D: { if (avail>=8){ int m=(base==0x25)?(hasHome?0:2):1; std::string a1,b1,a2,b2;   /* RgceArea: rwFirst,rwLast,colFirst,colLast */
                    render_loc2(u16(r),  u16(r+4), homeRow,homeCol,m,a1,b1);
                    render_loc2(u16(r+2),u16(r+6), homeRow,homeCol,m,a2,b2);
                    push(a1+":"+a2, b1+":"+b2, 8);} i += 8; } break;                                                  /* PtgArea / PtgAreaN */
                case 0x3A: { int m=hasHome?0:2; if (avail>=6){ uint16_t ix=u16(r); std::string a,b; render_loc2(u16(r+2),u16(r+4),homeRow,homeCol,m,a,b);
                    std::string q=sheet_qual(g,ix);
                    bool multi = ix < g.xtis.size() && g.xtis[ix].first != g.xtis[ix].last;   /* cross-sheet single ref -> area form A1:A1 */
                    if (multi) push(q+a+":"+a, q+b+":"+b, 8); else push(q+a,q+b);} i += 6; } break;                    /* PtgRef3d: ixti + RgceLoc */
                case 0x3B: { int m=hasHome?0:2; if (avail>=10){ uint16_t ix=u16(r); std::string a1,b1,a2,b2;         /* PtgArea3d: ixti + RgceArea */
                    render_loc2(u16(r+2),u16(r+6),homeRow,homeCol,m,a1,b1);
                    render_loc2(u16(r+4),u16(r+8),homeRow,homeCol,m,a2,b2);
                    std::string q=sheet_qual(g,ix); push(q+a1+":"+a2, q+b1+":"+b2, 8);} i += 10; } break;
                case 0x23: { uint32_t idx = avail>=4 ? (uint32_t(r[0])|(uint32_t(r[1])<<8)|(uint32_t(r[2])<<16)|(uint32_t(r[3])<<24)) : 0; i += 4;
                             std::string nm = (idx>=1 && idx<=g.lbl_names.size()) ? g.lbl_names[idx-1] : ("Name"+std::to_string(idx));
                             push(nm,nm); } break;                                       /* PtgName -> resolved defined-name */
                case 0x21: { uint16_t f = avail>=2 ? u16(r) : 0; i += 2;
                             const char* fn=ftab(f); std::string nm = fn ? fn : ("FUNC"+std::to_string(f)); func(nm.c_str(), 1); } break;   /* PtgFunc (fixed; argc best-effort 1) */
                case 0x22: { uint8_t argc = avail?r[0]:0; uint16_t f = avail>=3 ? u16(r+1) : 0; i += 3;
                             const char* fn=ftab(f); std::string nm = fn ? fn : ("FUNC"+std::to_string(f)); func(nm.c_str(), argc); } break; /* PtgFuncVar */
                default: i = cce; break;                                               /* unknown operand -> stop */
            }
        }
    }
    if (st.empty()) return {"", "", 99};
    return st.back();
}

}  // namespace

/* ── xls_names — defined-name inventory + rendered target formula ──────────────────────────────────── */
const char* bboxes_xls_names_json(const void* buf, size_t len) {
    static thread_local std::string out;
    json o; o["dialect"] = "xls";
    std::vector<char> wbuf; std::string err;
    if (!read_workbook_stream(buf, len, wbuf, err)) { o["names"]=json::array(); o["error"]=err;
        out = o.dump(-1,' ',false,json::error_handler_t::replace); return out.c_str(); }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(wbuf.data()); size_t n = wbuf.size();
    Globals g = scan_globals(p, n);
    json names = json::array(); size_t off = 0;
    while (off + 4 <= n) {
        uint16_t type = uint16_t(p[off]) | (uint16_t(p[off+1])<<8);
        uint16_t rlen = uint16_t(p[off+2]) | (uint16_t(p[off+3])<<8);
        off += 4; if (off + rlen > n) break;
        if (type == 0x002F) { o["names"]=json::array(); o["error"]="FILEPASS: encrypted"; out=o.dump(); return out.c_str(); }
        if (type == 0x0018 && rlen >= 15) {
            const uint8_t* r = p + off;
            uint16_t grbit = uint16_t(r[0])|(uint16_t(r[1])<<8);
            uint8_t cch = r[3]; uint16_t cce = uint16_t(r[4])|(uint16_t(r[5])<<8); uint16_t itab = uint16_t(r[8])|(uint16_t(r[9])<<8);
            bool hidden=(grbit&0x0001), builtin=(grbit&0x0020);
            uint8_t flags = r[14]; const uint8_t* s = r + 15; std::string name;
            if (builtin && cch>=1) { const char* bn=builtin_name(s[0]); name = bn?bn:("builtin#"+std::to_string(s[0])); }
            else if (flags&0x01) { for(uint32_t k=0;k<cch && 15u+2u*k+1u<rlen;k++){uint16_t ch=uint16_t(s[2*k])|(uint16_t(s[2*k+1])<<8); name+=(ch<128)?char(ch):'?';} }
            else { for(uint32_t k=0;k<cch && 15u+k<rlen;k++) name+=char(s[k]); }
            size_t namebytes = (flags&0x01)? cch*2 : cch;
            const uint8_t* rgce = s + namebytes; size_t rgce_avail = (15u+namebytes<=rlen)? rlen-(15u+namebytes) : 0;
            Expr tgt = (cce>0 && cce<=rgce_avail) ? render_rgce(rgce, cce, 0, 0, false, g) : Expr{};
            names.push_back({{"name",name},{"scope", itab==0?json(nullptr):json((int)itab-1)},
                             {"hidden",hidden},{"builtin",builtin},
                             {"formula_r1c1", tgt.r1c1.empty()?json(nullptr):json(tgt.r1c1)},
                             {"formula_a1",   tgt.a1.empty()?json(nullptr):json(tgt.a1)}});
        }
        off += rlen;
    }
    o["names"] = std::move(names);
    out = o.dump(-1,' ',false,json::error_handler_t::replace); return out.c_str();
}
const char* bboxes_xls_names_json_file(const char* path){ std::vector<char> s=slurp(path); return bboxes_xls_names_json(s.data(),s.size()); }

/* ── xls_formulas — every cell formula, its own address, both notations ────────────────────────────── */
const char* bboxes_xls_formulas_json(const void* buf, size_t len) {
    static thread_local std::string out;
    json o; o["dialect"] = "xls";
    std::vector<char> wbuf; std::string err;
    if (!read_workbook_stream(buf, len, wbuf, err)) { o["formulas"]=json::array(); o["error"]=err;
        out=o.dump(-1,' ',false,json::error_handler_t::replace); return out.c_str(); }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(wbuf.data()); size_t n = wbuf.size();
    Globals g = scan_globals(p, n);
    std::unordered_map<uint32_t,int> pos2idx;                 /* BoundSheet8 lbPlyPos -> libxls sheet index */
    for (size_t s = 0; s < g.sheet_pos.size(); s++) pos2idx[g.sheet_pos[s]] = (int)s;
    if (g.biff_version != 0x0600)
        o["warning"] = "BIFF5/7 workbook: 3D reference layout differs from BIFF8 and is not fully decoded";
    auto rd16 = [&](const uint8_t* q, size_t o2){ return uint16_t(q[o2] | (q[o2+1] << 8)); };
    auto shkey = [](int sh, int rw, int cl){ return (uint64_t(uint32_t(sh)) << 40) | (uint64_t(rw & 0xFFFF) << 16) | uint32_t(cl & 0xFFFF); };
    /* pass 1: ShrFmla (0x04BC) base rgce keyed by (sheet,topRow,topCol); every member cell (incl. master)
       carries a lone PtgExp pointing here. */
    std::unordered_map<uint64_t, std::vector<uint8_t>> shared;
    { size_t o2 = 0; int cs = -1;
      while (o2 + 4 <= n) {
          size_t rs = o2; uint16_t t = rd16(p, o2), l = rd16(p, o2 + 2); o2 += 4; if (o2 + l > n) break;
          const uint8_t* rr = p + o2;
          if (t == 0x0809) { auto it = pos2idx.find((uint32_t)rs); if (it != pos2idx.end()) cs = it->second; }
          else if (t == 0x04BC && l >= 10) {                 /* rwFirst2 rwLast2 colFirst1 colLast1 rsvd1 cUse1 cce2 rgce */
              int rf = rd16(rr, 0), cf = rr[4]; uint16_t cce = rd16(rr, 8);
              if (10u + cce <= l) shared[shkey(cs, rf, cf)] = std::vector<uint8_t>(rr + 10, rr + 10 + cce);
          }
          o2 += l;
      }
    }
    json arr = json::array(); size_t off = 0; int cur_sheet = -1;
    while (off + 4 <= n) {
        size_t rec_start = off;
        uint16_t type = uint16_t(p[off]) | (uint16_t(p[off+1])<<8);
        uint16_t rlen = uint16_t(p[off+2]) | (uint16_t(p[off+3])<<8);
        off += 4; if (off + rlen > n) break;
        const uint8_t* r = p + off;
        if (type == 0x002F) { o["formulas"]=json::array(); o["error"]="FILEPASS: encrypted"; out=o.dump(); return out.c_str(); }
        if (type == 0x0809) { auto it = pos2idx.find((uint32_t)rec_start);   /* align to BoundSheet8, robust to chart/macro sheets */
                              if (it != pos2idx.end()) cur_sheet = it->second; }
        if (type == 0x0006 && rlen >= 22) {                  /* Formula: rw,col,ixfe,num(8),grbit,chn,cce,rgce */
            int rw = uint16_t(r[0])|(uint16_t(r[1])<<8), col = uint16_t(r[2])|(uint16_t(r[3])<<8);
            uint16_t cce = uint16_t(r[20])|(uint16_t(r[21])<<8);
            const uint8_t* rgce = r + 22; size_t avail = rlen >= 22 ? rlen - 22 : 0;
            if (cce <= avail) {
                Expr e;
                if (cce >= 5 && rgce[0] == 0x01) {           /* PtgExp -> shared/array member: expand base at (topRow,topCol) */
                    int tr = rd16(rgce, 1), tc = rd16(rgce, 3);
                    auto it = shared.find(shkey(cur_sheet, tr, tc));
                    if (it != shared.end()) e = render_rgce(it->second.data(), it->second.size(), rw, col, true, g);
                } else {
                    e = render_rgce(rgce, cce, rw, col, true, g);
                }
                std::string addr_a1 = col_letters(col) + std::to_string(rw + 1);
                std::string addr_r1c1 = "R" + std::to_string(rw + 1) + "C" + std::to_string(col + 1);
                arr.push_back({{"sheet", cur_sheet}, {"row", rw}, {"col", col},
                               {"address_a1", addr_a1}, {"address_r1c1", addr_r1c1},
                               {"formula_a1", e.a1}, {"formula_r1c1", e.r1c1}});
            }
        }
        off += rlen;
    }
    o["formulas"] = std::move(arr);
    out = o.dump(-1,' ',false,json::error_handler_t::replace); return out.c_str();
}
const char* bboxes_xls_formulas_json_file(const char* path){ std::vector<char> s=slurp(path); return bboxes_xls_formulas_json(s.data(),s.size()); }
