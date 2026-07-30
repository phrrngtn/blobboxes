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

const c_flags: []const []const u8 = &.{"-std=c11"};
const cxx_flags: []const []const u8 = &.{"-std=c++17"};

/// The always-on extraction core. Backends are added on top.
const core_sources: []const []const u8 = &.{
    "src/bboxes_core.cpp",
    "src/bboxes_pdf.cpp",
    "src/bboxes_meta.cpp",
    "src/bboxes_xfdf.cpp",
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
        .name = "docx",
        .define = "BBOXES_HAS_DOCX",
        .sources = &.{"src/bboxes_docx.cpp"},
        .help = "DOCX backend (pugixml + miniz, both always linked)",
    },
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

    // PDFium: link the prebuilt shared library, and find it beside us at load.
    mod.addObjectFile(pdfiumLib(b, d.pdfium, t));
    mod.addRPathSpecial(if (t.os.tag == .macos) "@loader_path" else "$ORIGIN");

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

    const d: Deps = .{
        .pdfium = pdfium,
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
