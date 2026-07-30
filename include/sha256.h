/*
 * sha256.h — drop-in replacement for stbrumme/hash-library's SHA256.
 *
 * blobboxes used that library for one algorithm at four call sites, and paid a
 * GitHub dependency, a three-file static library, and a synthesised <endian.h>
 * for it on macOS (hash-library includes a header Darwin does not ship).
 * `std.crypto.hash.sha2` does the same job, so the implementation now lives in
 * src/bboxes_zig.zig and this header is the shim that keeps the call sites
 * unchanged.
 *
 * Only the calling convention actually used is provided:
 *
 *     SHA256 sha;
 *     std::string hex = sha(buffer, length);
 *
 * hash-library also offered incremental add()/getHash() and an std::string
 * overload. Those are deliberately absent rather than stubbed: nothing here
 * calls them, and an unused reimplementation is a thing to get subtly wrong.
 * Add them here, backed by a streaming variant of bb_sha256_hex, if a caller
 * ever needs them.
 */

#ifndef BBOXES_SHA256_H
#define BBOXES_SHA256_H

#include <cstddef>
#include <string>

extern "C" void bb_sha256_hex(const unsigned char *data, size_t len, char *out);

class SHA256 {
public:
    /* Lowercase hex, as hash-library produced. These digests are content
     * addresses that appear in extracted metadata (workbook_id, the document
     * checksum), so changing the case would silently invalidate every
     * identifier computed before now. */
    std::string operator()(const void *data, size_t len) const {
        char buf[65];
        bb_sha256_hex(static_cast<const unsigned char *>(data), len, buf);
        return std::string(buf, 64);
    }
};

#endif /* BBOXES_SHA256_H */
