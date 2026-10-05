// sha512.hpp : 独立实现的 SHA-512（用于下载文件的完整性校验）
// .NET 官方 release-metadata 的 files[].hash 为 SHA-512（128 个十六进制字符）
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

namespace sha512 {

inline constexpr uint64_t K[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full,
    0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull, 0x59f111f1b605d019ull,
    0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull,
    0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
    0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull,
    0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull, 0x2de92c6f592b0275ull,
    0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full,
    0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
    0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull,
    0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull,
    0x92722c851482353bull, 0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull,
    0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
    0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull,
    0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull,
    0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull,
    0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull,
    0xc67178f2e372532bull, 0xca273eceea26619cull, 0xd186b8c721c0c207ull,
    0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull,
    0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
    0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull,
    0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};

struct Ctx {
    uint64_t h[8];
    uint64_t len = 0; // 已处理字节数
    uint8_t buf[128];
    size_t buf_len = 0;
};

inline uint64_t rotr(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

inline void reset(Ctx& c) {
    static const uint64_t H0[8] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull,
                                   0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
                                   0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
                                   0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};
    memcpy(c.h, H0, sizeof(H0));
    c.len = 0;
    c.buf_len = 0;
}

inline void block(Ctx& c, const uint8_t* p) {
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint64_t)p[8 * i] << 56) | ((uint64_t)p[8 * i + 1] << 48) |
               ((uint64_t)p[8 * i + 2] << 40) | ((uint64_t)p[8 * i + 3] << 32) |
               ((uint64_t)p[8 * i + 4] << 24) | ((uint64_t)p[8 * i + 5] << 16) |
               ((uint64_t)p[8 * i + 6] << 8) | (uint64_t)p[8 * i + 7];
    }
    for (int i = 16; i < 80; ++i) {
        uint64_t s0 = rotr(w[i - 15], 1) ^ rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = rotr(w[i - 2], 19) ^ rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3];
    uint64_t e = c.h[4], f = c.h[5], g = c.h[6], h = c.h[7];
    for (int i = 0; i < 80; ++i) {
        uint64_t S1 = rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = h + S1 + ch + K[i] + w[i];
        uint64_t S0 = rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39);
        uint64_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint64_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d;
    c.h[4] += e; c.h[5] += f; c.h[6] += g; c.h[7] += h;
}

inline void update(Ctx& c, const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    c.len += n;
    while (n) {
        size_t take = 128 - c.buf_len;
        if (take > n) take = n;
        memcpy(c.buf + c.buf_len, p, take);
        c.buf_len += take;
        p += take;
        n -= take;
        if (c.buf_len == 128) {
            block(c, c.buf);
            c.buf_len = 0;
        }
    }
}

inline std::string hex(Ctx c) {
    // SHA-512 填充：0x80 → 0x00 至 len ≡ 112 (mod 128) → 128 位大端长度
    uint64_t bits_hi = (uint64_t)(c.len >> 61);        // 高位：len/8 的溢出部分
    uint64_t bits_lo = c.len << 3;                     // 低位：字节数 × 8
    uint8_t pad = 0x80;
    update(c, &pad, 1);
    uint8_t z = 0;
    while (c.buf_len != 112) update(c, &z, 1);
    uint8_t lb[16];
    for (int i = 0; i < 8; ++i) lb[i] = (uint8_t)(bits_hi >> (56 - 8 * i));
    for (int i = 0; i < 8; ++i) lb[8 + i] = (uint8_t)(bits_lo >> (56 - 8 * i));
    update(c, lb, 16);
    char out[129];
    for (int i = 0; i < 8; ++i) snprintf(out + 16 * i, 17, "%016llx", (unsigned long long)c.h[i]);
    return std::string(out, 128);
}

// 进度回调：done/total 为已处理/总字节数（total 未知时为 0）
using Progress = std::function<void(uint64_t, uint64_t)>;

// 计算文件摘要；失败时返回空串（可选进度回调，按读取块推进）
inline std::string file_hex(const std::wstring& path, const Progress& progress = {}) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return {};
    uint64_t total = 0;
    if (progress) {
        _fseeki64(f, 0, SEEK_END);
        total = (uint64_t)_ftelli64(f);
        _fseeki64(f, 0, SEEK_SET);
    }
    Ctx c;
    reset(c);
    std::string chunk(1 << 20, '\0');
    uint64_t done = 0;
    size_t n;
    while ((n = fread(chunk.data(), 1, chunk.size(), f)) > 0) {
        update(c, chunk.data(), n);
        done += n;
        if (progress) progress(done, total);
    }
    bool bad = ferror(f) != 0;
    fclose(f);
    if (bad) return {};
    return hex(c);
}

} // namespace sha512
