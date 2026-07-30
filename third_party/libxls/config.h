/*
 * config.h — hand-authored replacement for libxls's autotools-generated config.
 *
 * libxls is an autotools project; `./configure` normally writes this. Running
 * configure would mean shipping a foreign build system for five C files, so the
 * header is written by hand instead — the same treatment as
 * third_party/miniz/miniz_export.h and third_party/xlnt/.../xlnt_cmake_export.h,
 * per the generate-once-and-commit rule in blobzig/PLAN.md.
 *
 * This is cheap here because libxls consults only five macros, all of them
 * platform or packaging facts rather than build options, so one file with
 * platform conditionals serves every target.
 *
 * Note how that set was established: grepping `HAVE_` found three and missed
 * ICONV_CONST and PACKAGE_VERSION, which failed the build as undeclared
 * identifiers. The complete sweep is:
 *
 *     grep -rhoE '\b(PACKAGE[A-Z_]*|ICONV_CONST|HAVE_[A-Z_]+|SIZEOF_[A-Z_]+|WORDS_[A-Z_]+)\b' src include | sort -u
 *
 * Re-run that on version bumps. A newly-added HAVE_ macro is worse than these
 * two were: it silently takes the #else branch instead of failing to compile.
 */

#ifndef BBOXES_LIBXLS_CONFIG_H
#define BBOXES_LIBXLS_CONFIG_H

/* iconv, for translating legacy codepages (BIFF5 and earlier store text in a
 * workbook-declared codepage) into UTF-8. Present in libSystem on Darwin and
 * in glibc on Linux; on the latter it needs no extra library, on the former it
 * needs -liconv. Without this, xlstool.c falls back to a latin1-only path and
 * silently mangles anything else. */
#define HAVE_ICONV 1

#if defined(__APPLE__)

/* Darwin has both the xlocale.h header and the _l-suffixed conversions. */
#  define HAVE_XLOCALE_H 1
#  define HAVE_WCSTOMBS_L 1

#else

/* glibc folded xlocale.h into locale.h (removed as a separate header in 2.26)
 * and provides no wcstombs_l. libxls's #else branch uses uselocale() around
 * plain wcstombs(), which is correct and portable — just not thread-cheap.
 * Deliberately left undefined rather than guessed at. */

#endif

/* iconv's second parameter is `char **` on both Darwin's libiconv and glibc, so
 * no const qualifier is wanted. Systems whose iconv takes `const char **` need
 * this defined to `const`. */
#define ICONV_CONST

/* Reported by xls_getVersion(). Keep in step with the pinned tarball in
 * build.zig.zon — nothing enforces the correspondence. */
#define PACKAGE_VERSION "1.6.3"

#endif /* BBOXES_LIBXLS_CONFIG_H */
