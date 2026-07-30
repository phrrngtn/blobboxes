/*
 * pdfium_dyn.cpp — the dlopen loader behind include/pdfium_dyn.h.
 *
 * Defines one function pointer per PDFium entry point and resolves them all on
 * first use. See the header for why PDFium is loaded rather than linked.
 */

/* Must precede the include: this translation unit names the real PDFium
 * functions to take their addresses, so the call-site redirect has to be off.
 * With it on, `&::FPDF_InitLibrary` would rewrite to the pointer's own address. */
#define BBOXES_PDFIUM_NO_REDIRECT 1
#include "pdfium_dyn.h"

#include <dlfcn.h>

#include <mutex>
#include <string>

/* One definition per pointer, initialised null. */
#define BBOXES_PDFIUM_DEFN(name) decltype(&::name) bb_dyn_##name = nullptr;
BBOXES_PDFIUM_FUNCS(BBOXES_PDFIUM_DEFN)
#undef BBOXES_PDFIUM_DEFN

namespace {

#if defined(__APPLE__)
constexpr const char *kSoname = "libpdfium.dylib";
#else
constexpr const char *kSoname = "libpdfium.so";
#endif

std::mutex g_mutex;
bool g_tried = false;
bool g_ok = false;
std::string g_error;

/* Directory containing this shared library.
 *
 * dladdr on one of our own symbols is the portable way to ask "where am I?".
 * We need it because the extension is normally loaded by an absolute path and
 * its own directory is not on any search path, yet libpdfium is shipped
 * alongside it. This is what the rpath used to do when PDFium was linked. */
std::string self_dir() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void *>(&self_dir), &info) == 0 || !info.dli_fname)
        return {};
    const std::string path(info.dli_fname);
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

void *try_open(const std::string &path) {
    if (path.empty()) return nullptr;
    /* LOCAL, not GLOBAL: PDFium's symbols stay private to us rather than
     * entering the process-wide namespace, where they could collide with
     * another extension carrying its own copy. NOLOAD is not used — we do want
     * it loaded. */
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}

} /* namespace */

extern "C" int bb_pdfium_load(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_tried) return g_ok ? 1 : 0;
    g_tried = true;

    void *h = nullptr;

    /* 1. Explicit override, for unusual deployments and for testing. */
    if (const char *env = getenv("BBOXES_PDFIUM_PATH"))
        h = try_open(env);

    /* 2. Beside this library, which is where the build installs it. */
    if (!h) {
        const std::string dir = self_dir();
        if (!dir.empty()) h = try_open(dir + "/" + kSoname);
    }

    /* 3. Bare soname, so the system loader's own search applies (a
     *    distribution package, DYLD_LIBRARY_PATH, /usr/local/lib, ...). */
    if (!h) h = try_open(kSoname);

    if (!h) {
        const char *e = dlerror();
        g_error = std::string("could not load ") + kSoname +
                  " — the PDF backend needs it beside the extension, or set "
                  "BBOXES_PDFIUM_PATH. Every other format works without it. (" +
                  (e ? e : "no detail") + ")";
        return 0;
    }

    /* Resolve everything before publishing any of it: a half-resolved table
     * would crash later at an unrelated call site rather than failing here. */
    const char *missing = nullptr;
#define BBOXES_PDFIUM_RESOLVE(name)                                            \
    if (!missing) {                                                            \
        auto *p = dlsym(h, #name);                                             \
        if (!p) missing = #name;                                               \
        else bb_dyn_##name = reinterpret_cast<decltype(&::name)>(p);           \
    }
    BBOXES_PDFIUM_FUNCS(BBOXES_PDFIUM_RESOLVE)
#undef BBOXES_PDFIUM_RESOLVE

    if (missing) {
        /* Clear the pointers we did set, so nothing is callable. */
#define BBOXES_PDFIUM_CLEAR(name) bb_dyn_##name = nullptr;
        BBOXES_PDFIUM_FUNCS(BBOXES_PDFIUM_CLEAR)
#undef BBOXES_PDFIUM_CLEAR
        g_error = std::string("libpdfium is missing ") + missing +
                  " — the library was found but is not the expected build";
        dlclose(h);
        return 0;
    }

    /* Deliberately never dlclose'd on success. The handle lives for the
     * process: PDFium keeps global state that FPDF_DestroyLibrary tears down,
     * and unloading the code underneath a host that still holds our function
     * pointers is the hazard DF_1_NODELETE exists to prevent elsewhere. */
    g_ok = true;
    return 1;
}

extern "C" const char *bb_pdfium_error(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_error.empty() ? nullptr : g_error.c_str();
}
