//! blobboxes — bounding-box extraction from PDF/XLSX/DOCX/HTML/XLS/text,
//! for DuckDB / SQLite / Python.
//!
//! The largest CMakeLists in the family (317 lines) and the most dependencies,
//! but structurally the most straightforward of the three remaining repos: ten
//! C++ sources, five optional backends, and one fat library that upstream was
//! already handling correctly.
//!
//! ## PDFium needs no shim, unlike HiGHS in blobsolver
//!
//! Worth stating because it looks like the same situation and is not. PDFium is
//! far more C++ inside than HiGHS, yet causes none of the trouble, for two
//! reasons worth copying wherever possible:
//!
//! - its public headers (`fpdfview.h`, `fpdf_text.h`, `fpdf_edit.h`) are **pure
//!   C**, so nothing about its C++ ABI reaches our link line
//! - it ships **shared only** — no static variant exists in any release asset —
//!   so its C++ runtime is sealed inside the dylib by construction
//!
//! That is exactly the arrangement blobsolver had to *build* for HiGHS. A fat
//! C++ dependency is safe when someone has already put a C boundary around it;
//! PDFium is what that looks like when upstream did the work.
//!
//! The cost is the one shared-only libraries always carry: `libpdfium.dylib`
//! ships beside the extension and is found through an rpath. Converting the 35
//! `FPDF_*` entry points to a dlopen table would remove that, and is a
//! scheduled follow-up rather than part of this port.
//!
//! ## What is Zig-built, and what is not
//!
//! Fetched binary: PDFium.
//! Zig-built C/C++: pugixml (one file), miniz (one file).
//! Header-only: nlohmann/json.
//! Deleted: hash-library — one algorithm at four call sites, replaced by
//!   `std.crypto.hash.sha2` in `src/bboxes_zig.zig`, which also removes the
//!   `<endian.h>` shim CMake had to synthesise for it on macOS.

const std = @import("std");
const blobzig = @import("blobzig");
const lexbor_sources = @import("third_party/lexbor_sources.zig");

const c_flags: []const []const u8 = &.{"-std=c11"};
const cxx_flags: []const []const u8 = &.{"-std=c++17"};

/// The always-on extraction core. Backends are added on top.
const core_sources: []const []const u8 = &.{
    "src/bboxes_core.cpp",
    "src/bboxes_pdf.cpp",
    "src/bboxes_meta.cpp",
    "src/bboxes_xfdf.cpp",
    // The PDFium dlopen loader. Always compiled: bboxes_pdf.cpp is always
    // compiled, and it now reaches PDFium only through these pointers.
    "src/pdfium_dyn.cpp",
};

/// An optional backend: a `-D` and one or more sources.
///
/// Only the two dependency-free ones are wired up in this pass. XLSX (xlnt),
/// HTML (lexbor) and XLS (libxls) each need a dependency that is not yet
/// fetched, and are the next step — see ZIG_PORT_NOTES.md.
const Backend = struct {
    name: []const u8,
    define: []const u8,
    sources: []const []const u8,
    help: []const u8,
};

const backends: []const Backend = &.{
    .{
        .name = "text",
        .define = "BBOXES_HAS_TEXT",
        .sources = &.{"src/bboxes_text.cpp"},
        .help = "Plain-text backend (no extra dependencies)",
    },
    .{
        .name = "xlsx",
        .define = "BBOXES_HAS_XLSX",
        .sources = &.{"src/bboxes_xlsx.cpp"},
        .help = "XLSX backend (xlnt for fonts/styles, plus a pugixml fast path)",
    },
    .{
        .name = "html",
        .define = "BBOXES_HAS_HTML",
        .sources = &.{"src/bboxes_html.cpp"},
        .help = "HTML backend (lexbor)",
    },
    .{
        .name = "xls",
        .define = "BBOXES_HAS_XLS",
        // Two lanes: libxls for cells/colours, and our own BIFF walker over the
        // vendored compoundfilereader for formulas, defined names and VBA.
        .sources = &.{ "src/bboxes_xls.cpp", "src/bboxes_xls_biff.cpp" },
        .help = "Legacy .xls backend (libxls + our BIFF/OLE2 walker)",
    },
    .{
        .name = "docx",
        .define = "BBOXES_HAS_DOCX",
        .sources = &.{"src/bboxes_docx.cpp"},
        .help = "DOCX backend (pugixml + miniz, both always linked)",
    },
};

/// xlnt's sources, for the high-fidelity XLSX reader.
///
/// Enumerated rather than globbed, per PLAN.md. Tests and the Python bindings
/// are excluded; everything else upstream compiles into libxlnt is here.
const xlnt_sources: []const []const u8 = &.{
    "source/cell/cell.cpp",
    "source/cell/cell_reference.cpp",
    "source/cell/comment.cpp",
    "source/cell/hyperlink.cpp",
    "source/cell/index_types.cpp",
    "source/cell/phonetic_run.cpp",
    "source/cell/rich_text.cpp",
    "source/cell/rich_text_run.cpp",
    "source/detail/constants.cpp",
    "source/detail/cryptography/aes.cpp",
    "source/detail/cryptography/base64.cpp",
    "source/detail/cryptography/compound_document.cpp",
    "source/detail/cryptography/encryption_info.cpp",
    "source/detail/cryptography/hash.cpp",
    "source/detail/cryptography/sha.cpp",
    "source/detail/cryptography/xlsx_crypto_consumer.cpp",
    "source/detail/cryptography/xlsx_crypto_producer.cpp",
    "source/detail/header_footer/header_footer_code.cpp",
    "source/detail/number_format/number_formatter.cpp",
    "source/detail/serialization/custom_value_traits.cpp",
    "source/detail/serialization/open_stream.cpp",
    "source/detail/serialization/serialisation_helpers.cpp",
    "source/detail/serialization/vector_streambuf.cpp",
    "source/detail/serialization/xlsx_consumer.cpp",
    "source/detail/serialization/xlsx_producer.cpp",
    "source/detail/serialization/zstream.cpp",
    "source/detail/unicode.cpp",
    "source/detail/utils/string_helpers.cpp",
    "source/drawing/spreadsheet_drawing.cpp",
    "source/packaging/ext_list.cpp",
    "source/packaging/manifest.cpp",
    "source/packaging/relationship.cpp",
    "source/packaging/uri.cpp",
    "source/styles/alignment.cpp",
    "source/styles/border.cpp",
    "source/styles/color.cpp",
    "source/styles/conditional_format.cpp",
    "source/styles/fill.cpp",
    "source/styles/font.cpp",
    "source/styles/format.cpp",
    "source/styles/number_format.cpp",
    "source/styles/protection.cpp",
    "source/styles/style.cpp",
    "source/utils/date.cpp",
    "source/utils/datetime.cpp",
    "source/utils/exceptions.cpp",
    "source/utils/path.cpp",
    "source/utils/time.cpp",
    "source/utils/timedelta.cpp",
    "source/utils/variant.cpp",
    "source/workbook/named_range.cpp",
    "source/workbook/streaming_workbook_reader.cpp",
    "source/workbook/streaming_workbook_writer.cpp",
    "source/workbook/workbook.cpp",
    "source/workbook/worksheet_iterator.cpp",
    "source/worksheet/cell_iterator.cpp",
    "source/worksheet/cell_vector.cpp",
    "source/worksheet/header_footer.cpp",
    "source/worksheet/page_margins.cpp",
    "source/worksheet/page_setup.cpp",
    "source/worksheet/phonetic_pr.cpp",
    "source/worksheet/range.cpp",
    "source/worksheet/range_iterator.cpp",
    "source/worksheet/range_reference.cpp",
    "source/worksheet/selection.cpp",
    "source/worksheet/sheet_protection.cpp",
    "source/worksheet/worksheet.cpp",
};

/// xlnt's two C sources.
///
/// Separate from the list above because they are C, not C++, and an
/// enumeration that globbed only `*.cpp` silently dropped them — the build then
/// failed at link with undefined `sha1_hash`/`sha512_hash` rather than at
/// compile. "Enumerate, do not glob" only helps if the enumeration itself is
/// not a glob with the wrong pattern.
const xlnt_c_sources: []const []const u8 = &.{
    "source/detail/cryptography/sha1.c",
    "source/detail/cryptography/sha512.c",
};

/// libstudxml, xlnt's XML pull-parser, taken from its own build file rather
/// than guessed: four C++ sources, the genx serialiser, and a bundled expat.
///
/// This is the second XML parser in the binary (pugixml is the first) and expat
/// makes a third engine. That redundancy is inherited from xlnt, not chosen —
/// worth remembering if the xlnt reader is ever dropped in favour of the
/// pugixml fast path, which would remove all of it.
const libstudxml_sources: []const []const u8 = &.{
    "third-party/libstudxml/libstudxml/parser.cxx",
    "third-party/libstudxml/libstudxml/qname.cxx",
    "third-party/libstudxml/libstudxml/serializer.cxx",
    "third-party/libstudxml/libstudxml/value-traits.cxx",
};

const libstudxml_c_sources: []const []const u8 = &.{
    "third-party/libstudxml/libstudxml/details/genx/char-props.c",
    "third-party/libstudxml/libstudxml/details/genx/genx.c",
    "third-party/libstudxml/libstudxml/details/expat/xmlparse.c",
    "third-party/libstudxml/libstudxml/details/expat/xmlrole.c",
    "third-party/libstudxml/libstudxml/details/expat/xmltok.c",
};

/// libxls's library sources.
///
/// `xls2csv.c` is deliberately absent: it is upstream's command-line tool and
/// carries a `main`, which would collide with every host that loads us.
const libxls_sources: []const []const u8 = &.{
    "src/endian.c", "src/locale.c", "src/ole.c", "src/xls.c", "src/xlstool.c",
};

/// miniz's four translation units.
///
/// miniz 3.x is not the single-file library its reputation suggests — the
/// amalgamated `miniz.c` in the repository holds only the core, with deflate,
/// inflate and the zip reader in separate files. Linking `miniz.c` alone
/// compiles cleanly and then fails with fourteen undefined `mz_zip_*` and
/// `tdefl_*` symbols.
const miniz_sources: []const []const u8 = &.{
    "miniz.c", "miniz_tdef.c", "miniz_tinfl.c", "miniz_zip.c",
};

/// Pick the PDFium distribution matching the target.
///
/// One dependency per platform, each lazy, so a macOS build never downloads the
/// Linux tarballs. Per-target fetching is not optional: blobd2 showed that a
/// single prebuilt links "successfully" into an artifact broken everywhere else.
fn pdfiumDep(b: *std.Build, target: std.Target) ?*std.Build.Dependency {
    const name: ?[]const u8 = switch (target.os.tag) {
        .macos => switch (target.cpu.arch) {
            .aarch64 => "pdfium_macos_aarch64",
            else => null,
        },
        .linux => switch (target.cpu.arch) {
            .x86_64 => "pdfium_linux_x86_64",
            .aarch64 => "pdfium_linux_aarch64",
            else => null,
        },
        else => null,
    };
    const dep_name = name orelse {
        std.debug.print(
            \\blobboxes: no prebuilt PDFium for {s}-{s}.
            \\
            \\bblanchon/pdfium-binaries publishes many more platforms than are
            \\wired up here. Adding one means adding the release asset to
            \\build.zig.zon and a case above — PDFium is a Chromium GN build and
            \\is not realistically built from source.
            \\
        , .{ @tagName(target.cpu.arch), @tagName(target.os.tag) });
        std.process.exit(1);
    };
    return b.lazyDependency(dep_name, .{});
}

const Deps = struct {
    pdfium: *std.Build.Dependency,
    xlnt: ?*std.Build.Dependency,
    libxls: ?*std.Build.Dependency,
    lexbor: ?*std.Build.Dependency,
    json: *std.Build.Dependency,
    pugixml: *std.Build.Dependency,
    miniz: *std.Build.Dependency,
    enabled: []const *const Backend,
};

fn pdfiumLibName(target: std.Target) []const u8 {
    return if (target.os.tag == .macos) "libpdfium.dylib" else "libpdfium.so";
}

/// PDFium's dylib, with its install name corrected on macOS.
///
/// bblanchon's macOS build ships with an install name of `./libpdfium.dylib`
/// (confirmed with `otool -D`). That is a *relative* path, so it resolves only
/// when the loading process happens to have the library in its working
/// directory — every rpath entry is ignored, because a load command without
/// `@rpath` never consults them. CMake papered over this with an
/// `install_name_tool -id` call that mutated the downloaded file in place.
///
/// In place is not available here: `zig-pkg` is a content-addressed cache and
/// mutating it would poison the hash for every other consumer. So the fix
/// produces a corrected *copy* as a build output, and everything links and
/// installs that copy instead.
///
/// Linux needs none of this — ELF `SONAME` is already `libpdfium.so`, with no
/// path attached, and `$ORIGIN` applies normally.
fn pdfiumLib(b: *std.Build, pdfium: *std.Build.Dependency, target: std.Target) std.Build.LazyPath {
    const src = pdfium.path(b.fmt("lib/{s}", .{pdfiumLibName(target)}));
    if (target.os.tag != .macos) return src;

    const script =
        \\set -eu
        \\cp "$1" "$2"
        \\chmod u+w "$2"
        \\install_name_tool -id @rpath/libpdfium.dylib "$2"
    ;
    // $0 is the script name, so the file args below land as $1 and $2.
    const run = b.addSystemCommand(&.{ "sh", "-c", script, "fix-pdfium-install-name" });
    run.addFileArg(src);
    return run.addOutputFileArg("libpdfium.dylib");
}

/// Everything every artifact needs: the C++ core, its dependencies, and PDFium.
///
/// Each artifact compiles the sources rather than sharing one static library,
/// which is what the CMake build effectively did too.
fn addCore(b: *std.Build, mod: *std.Build.Module, d: Deps) void {
    const t = mod.resolved_target.?.result;

    mod.addIncludePath(b.path("include"));
    mod.addIncludePath(d.json.path("include"));
    mod.addIncludePath(d.pdfium.path("include"));

    for (core_sources) |src| {
        mod.addCSourceFile(.{ .file = b.path(src), .flags = cxx_flags });
    }

    for (d.enabled) |backend| {
        for (backend.sources) |src| {
            mod.addCSourceFile(.{ .file = b.path(src), .flags = cxx_flags });
        }
        mod.addCMacro(backend.define, "1");
    }

    // pugixml: one C++ file. No Zig-stdlib XML parser exists to replace it, and
    // four of the extractors depend on its DOM.
    mod.addIncludePath(d.pugixml.path("src"));
    mod.addCSourceFile(.{ .file = d.pugixml.path("src/pugixml.cpp"), .flags = cxx_flags });

    // miniz: one C file, the zip container behind DOCX and XLSX. `std.zip`
    // exists, but bboxes_docx.cpp uses the miniz API at many call sites, so
    // swapping it means rewriting that reader rather than changing a build rule.
    mod.addIncludePath(d.miniz.path("."));
    // miniz_export.h is generated by miniz's CMake; ours is committed. See the
    // header for why.
    mod.addIncludePath(b.path("third_party/miniz"));
    for (miniz_sources) |src| {
        mod.addCSourceFile(.{ .file = d.miniz.path(src), .flags = c_flags });
    }

    // xlnt, when the XLSX backend is on. Compiled into each artifact like
    // everything else rather than built as a separate library.
    if (d.xlnt) |xlnt| {
        mod.addIncludePath(xlnt.path("include"));
        // xlnt_cmake_export.h is produced by CMake's generate_export_header;
        // ours is committed. Must come before xlnt's own include dir would
        // otherwise look for it. See the header for why.
        mod.addIncludePath(b.path("third_party/xlnt"));
        // xlnt's own sources include its internal headers by path relative to
        // source/, and reach into the submodules directly.
        mod.addIncludePath(xlnt.path("source"));
        mod.addIncludePath(xlnt.path("third-party/libstudxml"));
        mod.addIncludePath(xlnt.path("third-party/utfcpp/source"));
        mod.addIncludePath(xlnt.path("third-party/fmt/include"));
        mod.addIncludePath(xlnt.path("third-party/fast_float/include"));

        // Deliberately NOT xlnt's third-party/miniz: it bundles miniz 2.x while
        // we compile 3.x, and both define the whole mz_* surface. Two copies in
        // one binary is a duplicate-symbol error at best and a silently chosen
        // winner at worst. Only zstream.cpp includes <miniz.h>, and the zip API
        // it uses is unchanged between the two, so it compiles against ours.
        mod.addIncludePath(d.miniz.path("."));

        // Static: no visibility attributes, nothing exported from a shared lib.
        mod.addCMacro("XLNT_STATIC", "1");

        for (xlnt_sources) |src| {
            mod.addCSourceFile(.{ .file = xlnt.path(src), .flags = cxx_flags });
        }
        for (xlnt_c_sources) |src| {
            mod.addCSourceFile(.{ .file = xlnt.path(src), .flags = c_flags });
        }
        for (libstudxml_sources) |src| {
            mod.addCSourceFile(.{ .file = xlnt.path(src), .flags = cxx_flags });
        }
        for (libstudxml_c_sources) |src| {
            mod.addCSourceFile(.{ .file = xlnt.path(src), .flags = c_flags });
        }
    }

    // libxls, when the legacy .xls backend is on.
    if (d.libxls) |libxls| {
        mod.addIncludePath(libxls.path("include"));
        // Hand-authored stand-in for the autotools config.h. See the header.
        mod.addIncludePath(b.path("third_party/libxls"));
        // The BIFF walker's OLE2/CFB reader, header-only and vendored.
        mod.addIncludePath(b.path("third_party/compoundfilereader"));
        for (libxls_sources) |src| {
            mod.addCSourceFile(.{ .file = libxls.path(src), .flags = c_flags });
        }
        // iconv translates the workbook-declared codepage of BIFF5 text into
        // UTF-8. glibc has it built in; on Darwin it is a separate library.
        if (t.os.tag == .macos) mod.linkSystemLibrary("iconv", .{});
    }

    // lexbor, when the HTML backend is on.
    if (d.lexbor) |lexbor| {
        mod.addIncludePath(lexbor.path("source"));
        for (lexbor_sources.common) |src| {
            mod.addCSourceFile(.{ .file = lexbor.path(src), .flags = c_flags });
        }
        const ports = if (t.os.tag == .windows)
            lexbor_sources.ports_windows
        else
            lexbor_sources.ports_posix;
        for (ports) |src| {
            mod.addCSourceFile(.{ .file = lexbor.path(src), .flags = c_flags });
        }
    }

    // PDFium: headers only. The library itself is dlopen'd at first use — see
    // include/pdfium_dyn.h. Nothing is linked, so the extension is a single
    // self-contained file that loads even when libpdfium is absent; only the
    // PDF backend then reports an error, and every other format still works.
    //
    // The install step below still ships libpdfium beside the artifacts, since
    // that is the first place the loader looks.

    mod.link_libcpp = true;
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const bz = b.dependency("blobzig", .{ .target = target, .optimize = optimize });
    const pdfium = pdfiumDep(b, target.result) orelse return;

    // CMake defaulted every backend OFF, which meant the documented configure
    // line had to re-enable them by hand and a plain build silently produced an
    // extension that could only read PDFs. On by default here: they are cheap,
    // and a format that fails to load is a worse surprise than a larger binary.
    var enabled: std.ArrayList(*const Backend) = .empty;
    for (backends) |*backend| {
        if (b.option(bool, backend.name, backend.help) orelse true) {
            enabled.append(b.allocator, backend) catch @panic("OOM");
        }
    }

    // Lazy: only fetched when the XLSX backend is actually on.
    var xlnt_dep: ?*std.Build.Dependency = null;
    for (enabled.items) |backend| {
        if (std.mem.eql(u8, backend.name, "xlsx")) {
            xlnt_dep = b.lazyDependency("xlnt", .{}) orelse return;
        }
    }

    var libxls_dep: ?*std.Build.Dependency = null;
    for (enabled.items) |backend| {
        if (std.mem.eql(u8, backend.name, "xls")) {
            libxls_dep = b.lazyDependency("libxls", .{}) orelse return;
        }
    }

    var lexbor_dep: ?*std.Build.Dependency = null;
    for (enabled.items) |backend| {
        if (std.mem.eql(u8, backend.name, "html")) {
            lexbor_dep = b.lazyDependency("lexbor", .{}) orelse return;
        }
    }

    const d: Deps = .{
        .pdfium = pdfium,
        .xlnt = xlnt_dep,
        .libxls = libxls_dep,
        .lexbor = lexbor_dep,
        .json = b.dependency("nlohmann_json", .{}),
        .pugixml = b.dependency("pugixml", .{}),
        .miniz = b.dependency("miniz", .{}),
        .enabled = enabled.items,
    };

    const glue = b.path("src/bboxes_zig.zig");

    // ── The core, as the cdylib's root ────────────────────────────────
    const core = b.createModule(.{
        .root_source_file = glue,
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    addCore(b, core, d);

    // ── DuckDB shim ───────────────────────────────────────────────────
    const duckdb_mod = b.createModule(.{
        .root_source_file = glue,
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    addCore(b, duckdb_mod, d);
    duckdb_mod.addIncludePath(bz.namedLazyPath("duckdb_capi_include"));
    duckdb_mod.addCSourceFile(.{
        .file = b.path("duckdb_ext/src/bboxes_ext.cpp"),
        .flags = cxx_flags,
    });

    // ── SQLite shim ───────────────────────────────────────────────────
    const sqlite_mod = b.createModule(.{
        .root_source_file = glue,
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    addCore(b, sqlite_mod, d);
    sqlite_mod.addIncludePath(bz.namedLazyPath("sqlite_include"));
    sqlite_mod.addCSourceFile(.{
        .file = b.path("sqlite_ext/src/bboxes_sqlite.cpp"),
        .flags = cxx_flags,
    });

    const artifacts = blobzig.addHostExtensions(b, bz, .{
        .name = "bboxes",
        .target = target,
        .optimize = optimize,
        .core = core,
        .duckdb_module = duckdb_mod,
        .sqlite_module = sqlite_mod,
        // PDFium resolves through its own LC_LOAD_DYLIB / DT_NEEDED rather than
        // being linked in. Listing the prefix is the portability caveat: it
        // says "libpdfium must ship beside this artifact".
        .allow_undefined = &.{"FPDF"},
    });
    artifacts.lib.?.installHeader(b.path("include/bboxes.h"), "bboxes.h");

    // Ship libpdfium beside the artifacts that load it. Without this everything
    // builds and installs cleanly and fails at load, which is the worst place to
    // find out a file is missing.
    b.getInstallStep().dependOn(&b.addInstallFileWithDir(
        pdfiumLib(b, pdfium, target.result),
        .lib,
        pdfiumLibName(target.result),
    ).step);

    // ── C++ example, which doubles as the core smoke test ─────────────
    const example = b.addExecutable(.{
        .name = "bboxes_example",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });
    example.root_module.addCSourceFile(.{ .file = b.path("example/main.cpp"), .flags = cxx_flags });
    example.root_module.addIncludePath(b.path("include"));
    example.root_module.linkLibrary(artifacts.lib.?);
    example.root_module.link_libcpp = true;
    b.installArtifact(example);

    // ── Zig unit tests (the SHA-256 replacement has known-answer tests) ─
    const unit = b.addTest(.{ .root_module = core });
    const run_unit = b.addRunArtifact(unit);
    // The test binary runs out of the cache directory, where libpdfium is not.
    // Point it at the install tree and make sure that tree exists first, which
    // mirrors what a deployed extension actually needs rather than hiding it.
    run_unit.step.dependOn(b.getInstallStep());
    run_unit.setEnvironmentVariable(
        if (target.result.os.tag == .macos) "DYLD_LIBRARY_PATH" else "LD_LIBRARY_PATH",
        b.getInstallPath(.lib, ""),
    );
    b.step("test", "Run Zig unit tests").dependOn(&run_unit.step);
}
