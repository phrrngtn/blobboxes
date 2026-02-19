/*
 * Minimal MD5 implementation (public domain).
 * Based on the RSA Data Security reference implementation.
 * Returns a 32-char lowercase hex string.
 */

#ifndef BBOXES_MD5_H
#define BBOXES_MD5_H

#include <cstdint>
#include <cstring>
#include <string>

namespace bboxes_md5 {

static inline uint32_t F(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
static inline uint32_t G(uint32_t x, uint32_t y, uint32_t z) { return (x & z) | (y & ~z); }
static inline uint32_t H(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
static inline uint32_t I(uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); }
static inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static inline void transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t M[16];
    for (int i = 0; i < 16; i++)
        M[i] = uint32_t(block[i*4]) | (uint32_t(block[i*4+1]) << 8) |
               (uint32_t(block[i*4+2]) << 16) | (uint32_t(block[i*4+3]) << 24);

    static const uint32_t T[] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const int s[] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
                            5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                            4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
                            6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
    static const int g[] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
                            1,6,11,0,5,10,15,4,9,14,3,8,13,2,7,12,
                            5,8,11,14,1,4,7,10,13,0,3,6,9,12,15,2,
                            0,7,14,5,12,3,10,1,8,15,6,13,4,11,2,9};

    for (int i = 0; i < 64; i++) {
        uint32_t f, gi = M[g[i]];
        if (i < 16) f = F(b,c,d);
        else if (i < 32) f = G(b,c,d);
        else if (i < 48) f = H(b,c,d);
        else f = I(b,c,d);
        uint32_t temp = d;
        d = c; c = b;
        b = b + rotl(a + f + T[i] + gi, s[i]);
        a = temp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

static inline std::string compute(const void* data, size_t len) {
    uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const auto* input = static_cast<const uint8_t*>(data);

    size_t i = 0;
    for (; i + 64 <= len; i += 64)
        transform(state, input + i);

    uint8_t buf[128];
    size_t rem = len - i;
    std::memcpy(buf, input + i, rem);
    buf[rem++] = 0x80;
    size_t pad_to = (rem <= 56) ? 56 : 120;
    std::memset(buf + rem, 0, pad_to - rem);
    uint64_t bits = uint64_t(len) * 8;
    for (int j = 0; j < 8; j++)
        buf[pad_to + j] = uint8_t(bits >> (j * 8));
    transform(state, buf);
    if (pad_to > 56)
        transform(state, buf + 64);

    char hex[33];
    for (int j = 0; j < 4; j++) {
        uint32_t s = state[j];
        for (int k = 0; k < 4; k++) {
            uint8_t byte = uint8_t(s >> (k * 8));
            hex[j*8 + k*2]     = "0123456789abcdef"[byte >> 4];
            hex[j*8 + k*2 + 1] = "0123456789abcdef"[byte & 0xf];
        }
    }
    hex[32] = '\0';
    return std::string(hex);
}

} // namespace bboxes_md5

#endif
