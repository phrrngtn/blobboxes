/*
 * xlnt_cmake_export.h — hand-authored replacement for CMake's
 * generate_export_header, which xlnt's build invokes as:
 *
 *     GENERATE_EXPORT_HEADER (xlnt
 *         BASE_NAME xlnt_cmake
 *         EXPORT_MACRO_NAME XLNT_API
 *         DEPRECATED_MACRO_NAME XLNT_DEPRECATED
 *         EXPORT_FILE_NAME .../xlnt/utils/xlnt_cmake_export.h)
 *
 * `xlnt/xlnt_config.hpp` includes this unconditionally, and 122 declarations
 * across the public headers are marked XLNT_API, so nothing compiles without
 * it.
 *
 * We compile xlnt's sources straight into each artifact, so there is no shared
 * library to export from and every macro here is empty. Note this differs from
 * what generate_export_header would emit for the *default* CMake build, which
 * builds xlnt shared and fills XLNT_API with visibility attributes — if xlnt is
 * ever switched to a shared library here, this file has to grow those.
 *
 * Committed rather than generated, per the sanity rule in blobzig/PLAN.md:
 * generate with Zig tools, or generate once and commit. The same treatment as
 * third_party/miniz/miniz_export.h.
 */

#ifndef XLNT_CMAKE_EXPORT_H
#define XLNT_CMAKE_EXPORT_H

/* Compiled into the consumer, never exported from a shared library. */
#define XLNT_API
#define XLNT_CMAKE_NO_EXPORT

#ifndef XLNT_DEPRECATED
#  define XLNT_DEPRECATED __attribute__((__deprecated__))
#endif

#ifndef XLNT_DEPRECATED_EXPORT
#  define XLNT_DEPRECATED_EXPORT XLNT_API XLNT_DEPRECATED
#endif

#ifndef XLNT_DEPRECATED_NO_EXPORT
#  define XLNT_DEPRECATED_NO_EXPORT XLNT_CMAKE_NO_EXPORT XLNT_DEPRECATED
#endif

#endif /* XLNT_CMAKE_EXPORT_H */
