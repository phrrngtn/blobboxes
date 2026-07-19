// XFDF overlay emitter — the inverse of extraction.
//
// Input: JSON describing classification annotations (a rectangle + label per
// finding), typically built set-wise from a DB table of findings JOINed to page
// heights. Output: an XFDF document (Adobe's XML annotation-interchange format)
// that a PDF viewer loads as an *overlay* — the annotations render on top of the
// PDF without modifying the file. XFDF's <annots> (square/highlight/freetext/...)
// is a first-class annotation channel, distinct from its form <fields> side.
//
// Coordinate flip (the crux): the bboxes we EXTRACT are top-left origin (the
// extractor mapped PDFium's bottom-left space via page_height - top). XFDF rect
// is PDF *default user space* — bottom-left origin — so we flip back per annot
// using page_height. Pass origin:"bottom-left" to skip the flip.
#include "bboxes.h"
#include <nlohmann/json.hpp>
#include <string>
#include <cstdio>

using json = nlohmann::json;

namespace {

thread_local std::string g_xfdf;

void xml_escape(const std::string& s, std::string& out) {
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;        break;
        }
    }
}
std::string esc(const std::string& s) { std::string o; xml_escape(s, o); return o; }

std::string fmt4(double a, double b, double c, double d) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.3f,%.3f,%.3f,%.3f", a, b, c, d);
    return buf;
}

// Only these markup types are allowed in the element-name position (the type
// comes from untrusted JSON and becomes an XML tag — whitelist to be safe).
bool allowed_type(const std::string& t) {
    return t == "square" || t == "highlight" || t == "freetext" || t == "text" ||
           t == "circle" || t == "line" || t == "ink" || t == "polygon" ||
           t == "underline" || t == "strikeout";
}

} // namespace

extern "C" const char* bboxes_xfdf_from_json(const char* input) {
    json in = json::parse(input ? input : "", nullptr, false);

    std::string out;
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\" xml:space=\"preserve\">\n";

    const json* annots = nullptr;
    if (in.is_array()) {
        annots = &in;
    } else if (in.is_object()) {
        if (in.contains("href") && in["href"].is_string())
            out += "  <f href=\"" + esc(in["href"].get<std::string>()) + "\"/>\n";
        if (in.contains("annots") && in["annots"].is_array())
            annots = &in["annots"];
    }

    out += "  <annots>\n";
    if (annots) {
        for (const auto& a : *annots) {
            if (!a.is_object()) continue;
            std::string type = a.value("type", std::string("square"));
            if (!allowed_type(type)) type = "square";

            int page = a.value("page", 1);              // input is 1-based
            int xpage = page > 0 ? page - 1 : 0;         // XFDF is 0-based

            double x = a.value("x", 0.0), y = a.value("y", 0.0);
            double w = a.value("w", 0.0), h = a.value("h", 0.0);
            std::string origin = a.value("origin", std::string("top-left"));
            double x0, y0, x1, y1;
            if (origin == "bottom-left") {               // already PDF space
                x0 = x; y0 = y; x1 = x + w; y1 = y + h;
            } else {                                     // flip top-left → bottom-left
                double H = a.value("page_height", 0.0);
                x0 = x; y0 = H - (y + h); x1 = x + w; y1 = H - y;
            }

            std::string color   = a.value("color", std::string("#FF0000"));
            std::string title   = a.value("title", std::string("blobboxes"));
            std::string subject = a.value("subject", std::string());
            std::string contents = a.value("contents", std::string());

            out += "    <" + type + " page=\"" + std::to_string(xpage) + "\"";
            out += " rect=\"" + fmt4(x0, y0, x1, y1) + "\"";
            out += " color=\"" + esc(color) + "\"";
            if (a.contains("interior_color") && a["interior_color"].is_string())
                out += " interior-color=\"" + esc(a["interior_color"].get<std::string>()) + "\"";
            out += " title=\"" + esc(title) + "\"";
            if (!subject.empty()) out += " subject=\"" + esc(subject) + "\"";
            if (a.contains("opacity") && a["opacity"].is_number()) {
                char ob[16]; std::snprintf(ob, sizeof(ob), "%.3f", a["opacity"].get<double>());
                out += " opacity=\"" + std::string(ob) + "\"";
            }
            if (a.contains("name") && a["name"].is_string())
                out += " name=\"" + esc(a["name"].get<std::string>()) + "\"";
            if (a.contains("creationdate") && a["creationdate"].is_string())
                out += " creationdate=\"" + esc(a["creationdate"].get<std::string>()) + "\"";
            if (a.contains("date") && a["date"].is_string())
                out += " date=\"" + esc(a["date"].get<std::string>()) + "\"";
            out += " flags=\"print\"";
            // text-markup annotations (highlight/underline/strikeout) need QuadPoints
            if (type == "highlight" || type == "underline" || type == "strikeout")
                out += " coords=\"" + [&]{
                    char q[256];
                    std::snprintf(q, sizeof(q), "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f",
                                  x0, y1, x1, y1, x0, y0, x1, y0);   // ul, ur, ll, lr
                    return std::string(q);
                }() + "\"";
            out += ">";
            if (!contents.empty()) { out += "<contents>"; xml_escape(contents, out); out += "</contents>"; }
            out += "</" + type + ">\n";
        }
    }
    out += "  </annots>\n</xfdf>\n";

    g_xfdf = out;
    return g_xfdf.c_str();
}
