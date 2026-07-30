//! Zig glue for blobboxes — the module root the C++ backends are compiled into.
//!
//! The extraction backends stay C++ because that is where the fat libraries
//! live (PDFium, xlnt, lexbor, libxls) and because pugixml's DOM has no Zig
//! equivalent. What moves here is the work the Zig standard library already
//! does, so that a vendored C or C++ dependency can be deleted outright rather
//! than compiled.
//!
//! First tenant: SHA-256. blobboxes used stbrumme/hash-library for exactly one
//! algorithm at four call sites, and paid for it with a GitHub dependency, a
//! three-file static library, and a synthesised `<endian.h>` on macOS (the
//! library includes a header Darwin does not have). `std.crypto.hash.sha2` is
//! in the box.
//!
//! `include/sha256.h` presents the same `SHA256` type the call sites already
//! use, so none of them changed.

const std = @import("std");

/// Hex-encode the SHA-256 of a buffer into `out`, which must have room for 64
/// characters plus a NUL.
///
/// Lowercase hex, matching hash-library's output, because these digests are
/// content addresses that appear in extracted metadata (`workbook_id`, the
/// document checksum). A case change would silently invalidate every previously
/// computed identifier.
export fn bb_sha256_hex(data: ?[*]const u8, len: usize, out: [*]u8) void {
    var digest: [std.crypto.hash.sha2.Sha256.digest_length]u8 = undefined;

    // A null pointer with zero length is the hash of the empty string, which is
    // well-defined and is what hash-library returned for an empty buffer.
    const slice = if (data) |p| p[0..len] else &[_]u8{};
    std.crypto.hash.sha2.Sha256.hash(slice, &digest, .{});

    _ = std.fmt.bufPrint(out[0..64], "{x}", .{&digest}) catch unreachable;
    out[64] = 0;
}

test "sha256 matches the known empty-string digest" {
    var buf: [65]u8 = undefined;
    bb_sha256_hex(null, 0, &buf);
    try std.testing.expectEqualStrings(
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        buf[0..64],
    );
}

test "sha256 matches the known digest for 'abc'" {
    var buf: [65]u8 = undefined;
    bb_sha256_hex("abc", 3, &buf);
    try std.testing.expectEqualStrings(
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        buf[0..64],
    );
}
