// md5.hpp : 独立实现的 MD5（Python 旧版本发行文件只有 md5_sum 校验值）
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace md5 {

struct Ctx {
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    uint64_t len = 0;
    uint8_t buf[64];
    size_t buf_len = 0;
};

inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

inline void block(Ctx& c, const uint8_t* p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
        0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
        0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
        0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
        0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
        0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
        0xeb86d391};
    static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) | ((uint32_t)p[4 * i + 2] << 16) |
               ((uint32_t)p[4 * i + 3] << 24);
    uint32_t a = c.a, b = c.b, cc = c.c, d = c.d;
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & cc) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & cc);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ cc ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = cc ^ (b | ~d);
            g = (7 * i) % 16;
        }
        uint32_t tmp = d;
        d = cc;
        cc = b;
        b = b + rotl(a + f + K[i] + m[g], S[i]);
        a = tmp;
    }
    c.a += a;
    c.b += b;
    c.c += cc;
    c.d += d;
}

inline void update(Ctx& c, const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    c.len += n;
    while (n) {
        size_t take = 64 - c.buf_len;
        if (take > n) take = n;
        memcpy(c.buf + c.buf_len, p, take);
        c.buf_len += take;
        p += take;
        n -= take;
        if (c.buf_len == 64) {
            block(c, c.buf);
            c.buf_len = 0;
        }
    }
}

inline std::string hex(Ctx c) {
    uint64_t bits = c.len * 8;
    uint8_t pad = 0x80;
    update(c, &pad, 1);
    uint8_t z = 0;
    while (c.buf_len != 56) update(c, &z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; ++i) lb[i] = (uint8_t)(bits >> (8 * i)); // 小端长度
    update(c, lb, 8);
    static const char* hexd = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    uint32_t regs[4] = {c.a, c.b, c.c, c.d};
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k) {
            out += hexd[(regs[i] >> (8 * k + 4)) & 0xF];
            out += hexd[(regs[i] >> (8 * k)) & 0xF];
        }
    return out;
}

// 计算文件摘要；失败时返回空串
inline std::string file_hex(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return {};
    Ctx c;
    std::string chunk(1 << 20, '\0');
    size_t n;
    while ((n = fread(chunk.data(), 1, chunk.size(), f)) > 0) update(c, chunk.data(), n);
    bool bad = ferror(f) != 0;
    fclose(f);
    if (bad) return {};
    return hex(c);
}

} // namespace md5
