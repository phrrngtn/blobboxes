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
struct Globals { std::vector<std::string> sheets; std::vector<int> xti_first; };
Globals scan_globals(const uint8_t* p, size_t n) {
    Globals g; size_t off = 0;
    while (off + 4 <= n) {
        uint16_t type = uint16_t(p[off]) | (uint16_t(p[off+1]) << 8);
        uint16_t len  = uint16_t(p[off+2]) | (uint16_t(p[off+3]) << 8);
        off += 4; if (off + len > n) break;
        const uint8_t* r = p + off;
        if (type == 0x0085 && len >= 8) {                    /* BoundSheet8: name at offset 6 (ShortXLUnicodeString) */
            uint8_t cch = r[6], flags = r[7]; std::string nm;
            for (uint32_t i = 0; i < cch; i++) {
                if (flags & 0x01) { size_t o = 8 + 2*i; if (o+1 < len) nm += char(uint16_t(r[o]) | (uint16_t(r[o+1])<<8)); }
                else              { size_t o = 8 + i;   if (o   < len) nm += char(r[o]); }
            }
            g.sheets.push_back(nm);
        } else if (type == 0x0017 && len >= 2) {             /* ExternSheet: cXTI then {iSupBook,itabFirst,itabLast} */
            uint16_t cxti = uint16_t(r[0]) | (uint16_t(r[1]) << 8);
            for (uint16_t i = 0; i < cxti && 2 + i*6 + 5 < len; i++)
                g.xti_first.push_back(int16_t(uint16_t(r[2+i*6+2]) | (uint16_t(r[2+i*6+3]) << 8)));
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
        case 117: return "RIGHT"; case 118: return "MID"; case 119: return "TEXT"; case 148: return "TRIM";
        case 162: return "CLEAN"; case 190: return "ISNUMBER"; case 197: return "TRUNC"; case 212: return "ROUNDUP";
        case 213: return "ROUNDDOWN"; case 216: return "RANK"; case 219: return "ADDRESS"; case 228: return "SUMPRODUCT";
        case 252: return "COUNTIF"; case 269: return "AVERAGEA"; case 336: return "CONCATENATE"; case 345: return "SUMIFS";
        default: return nullptr;
    }
}
/* render one RgceLoc (rw + column-with-flags). hasHome: formula cell (rel = target-home); else name (rel = stored offset). */
void render_loc2(uint16_t rwf, uint16_t gc, int homeRow, int homeCol, bool hasHome, std::string& r1c1, std::string& a1) {
    int col14 = gc & 0x3FFF; bool colRel = (gc & 0x4000) != 0, rowRel = (gc & 0x8000) != 0;
    // R1C1: relative axis -> R[offset] (offset = target-home for formulas, stored signed offset for names)
    std::string rp, cp;
    if (rowRel) { int off = hasHome ? int(rwf) - homeRow : int(int16_t(rwf));
                  rp = "R" + (off ? "[" + std::to_string(off) + "]" : ""); }
    else        { rp = "R" + std::to_string(int(rwf) + 1); }
    if (colRel) { int off = hasHome ? col14 - homeCol : ((col14 & 0x2000) ? col14 - 0x4000 : col14);
                  cp = "C" + (off ? "[" + std::to_string(off) + "]" : ""); }
    else        { cp = "C" + std::to_string(col14 + 1); }
    r1c1 = rp + cp;
    // A1: absolute target address; $ on absolute axes (names' relative refs are best-effort in A1)
    a1 = (colRel ? "" : "$") + col_letters(col14) + (rowRel ? "" : "$") + std::to_string(int(rwf) + 1);
}
std::string sheet_qual(const Globals& g, uint16_t ixti) {
    if (ixti < g.xti_first.size()) {
        int s = g.xti_first[ixti];
        if (s >= 0 && s < (int)g.sheets.size()) {
            std::string nm = g.sheets[s];
            bool needq = nm.find(' ') != std::string::npos;
            return (needq ? "'" + nm + "'" : nm) + "!";
        }
    }
    return "";
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
                case 0x19: { uint8_t grbit = avail ? r[0] : 0; i += 3;                 /* PtgAttr */
                             if (grbit & 0x10) func("SUM", 1); } break;                 /* tAttrSum: optimized single-arg SUM */
                case 0x1C: { uint8_t e = avail?r[0]:0; i += 1; const char* s = e==0x17?"#REF!":e==0x07?"#DIV/0!":e==0x0F?"#VALUE!":e==0x1D?"#NAME?":e==0x24?"#N/A":"#ERR!"; push(s,s);} break;
                case 0x1D: { std::string s = (avail && r[0])?"TRUE":"FALSE"; i += 1; push(s,s);} break;   /* bool */
                case 0x1E: { int v = avail>=2 ? (uint16_t(r[0])|(uint16_t(r[1])<<8)) : 0; i += 2; push(std::to_string(v),std::to_string(v)); } break; /* int */
                case 0x1F: { double d=0; if(avail>=8) memcpy(&d,r,8); i += 8; char b[32]; snprintf(b,sizeof b,"%.15g",d); push(b,b);} break; /* num */
                default: i = cce; break;                                               /* unknown control -> stop */
            }
        } else {
            switch (base) {
                case 0x24: { if (avail>=4){ std::string a,b; render_loc2(u16(r),u16(r+2),homeRow,homeCol,hasHome,a,b); push(a,b);} i += 4; } break;      /* PtgRef */
                case 0x2C: { if (avail>=4){ std::string a,b; render_loc2(u16(r),u16(r+2),homeRow,homeCol,false,a,b);  push(a,b);} i += 4; } break;      /* PtgRefN (offset) */
                case 0x25: case 0x2D: { if (avail>=8){ bool hh=(base==0x25)?hasHome:false; std::string a1,b1,a2,b2;    /* RgceArea: rwFirst,rwLast,colFirst,colLast */
                    render_loc2(u16(r),  u16(r+4), homeRow,homeCol,hh,a1,b1);
                    render_loc2(u16(r+2),u16(r+6), homeRow,homeCol,hh,a2,b2);
                    push(a1+":"+a2, b1+":"+b2, 8);} i += 8; } break;                                                  /* PtgArea / PtgAreaN */
                case 0x3A: { if (avail>=6){ uint16_t ix=u16(r); std::string a,b; render_loc2(u16(r+2),u16(r+4),homeRow,homeCol,hasHome,a,b);
                    std::string q=sheet_qual(g,ix); push(q+a,q+b);} i += 6; } break;                                  /* PtgRef3d: ixti + RgceLoc */
                case 0x3B: { if (avail>=10){ uint16_t ix=u16(r); std::string a1,b1,a2,b2;                             /* PtgArea3d: ixti + RgceArea */
                    render_loc2(u16(r+2),u16(r+6),homeRow,homeCol,hasHome,a1,b1);
                    render_loc2(u16(r+4),u16(r+8),homeRow,homeCol,hasHome,a2,b2);
                    std::string q=sheet_qual(g,ix); push(q+a1+":"+a2, q+b1+":"+b2, 8);} i += 10; } break;
                case 0x23: { i += 4; push("Name#","Name#"); } break;                   /* PtgName (index; text resolved elsewhere) */
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
    json arr = json::array(); size_t off = 0; int cur_sheet = -1;
    while (off + 4 <= n) {
        uint16_t type = uint16_t(p[off]) | (uint16_t(p[off+1])<<8);
        uint16_t rlen = uint16_t(p[off+2]) | (uint16_t(p[off+3])<<8);
        off += 4; if (off + rlen > n) break;
        const uint8_t* r = p + off;
        if (type == 0x002F) { o["formulas"]=json::array(); o["error"]="FILEPASS: encrypted"; out=o.dump(); return out.c_str(); }
        if (type == 0x0809 && rlen >= 4) { uint16_t dt = uint16_t(r[2])|(uint16_t(r[3])<<8); if (dt==0x0010) cur_sheet++; }
        if (type == 0x0006 && rlen >= 22) {                  /* Formula: rw,col,ixfe,num(8),grbit,chn,cce,rgce */
            int rw = uint16_t(r[0])|(uint16_t(r[1])<<8), col = uint16_t(r[2])|(uint16_t(r[3])<<8);
            uint16_t cce = uint16_t(r[20])|(uint16_t(r[21])<<8);
            const uint8_t* rgce = r + 22; size_t avail = rlen >= 22 ? rlen - 22 : 0;
            if (cce <= avail) {
                Expr e = render_rgce(rgce, cce, rw, col, true, g);
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
