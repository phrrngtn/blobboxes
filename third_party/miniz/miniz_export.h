/*
 * miniz_export.h — hand-authored replacement for CMake's generate_export_header.
 *
 * miniz's CMakeLists calls generate_export_header(miniz), which writes this file
 * at configure time. We build miniz as one C file compiled straight into each
 * artifact, so there is no shared library to export from and every macro below
 * is empty.
 *
 * Committed rather than generated, per the sanity rule in blobzig/PLAN.md:
 * generate with Zig tools, or generate once and commit. Regenerating a
 * four-macro header at every build is not worth a CMake invocation.
 *
 * This matches what generate_export_header emits for a static build. If miniz
 * is ever switched to a shared library, this file has to grow real
 * visibility attributes — check upstream's generated output rather than
 * guessing.
 */

#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H

/* Built into the consumer, never exported from a shared library. */
#define MINIZ_EXPORT
#define MINIZ_NO_EXPORT

#ifndef MINIZ_DEPRECATED
#  define MINIZ_DEPRECATED __attribute__((__deprecated__))
#endif

#ifndef MINIZ_DEPRECATED_EXPORT
#  define MINIZ_DEPRECATED_EXPORT MINIZ_EXPORT MINIZ_DEPRECATED
#endif

#ifndef MINIZ_DEPRECATED_NO_EXPORT
#  define MINIZ_DEPRECATED_NO_EXPORT MINIZ_NO_EXPORT MINIZ_DEPRECATED
#endif

#endif /* MINIZ_EXPORT_H */
