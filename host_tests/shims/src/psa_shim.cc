// Host shim implementing the PSA Crypto subset from shims/psa/crypto.h.
// Software SHA-256, MD5, HMAC-SHA256 and AES-128 (ECB/CTR/GCM); the repo's
// golden-vector self-tests pin every primitive's correctness.

#include "psa/crypto.h"

#include <cstring>

#include <string>
#include <vector>

namespace {

// ---- SHA-256 ----

constexpr uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

uint32_t Ror32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

void Sha256Compress(uint32_t h[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (unsigned i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[4 * i]) << 24) |
               (static_cast<uint32_t>(block[4 * i + 1]) << 16) |
               (static_cast<uint32_t>(block[4 * i + 2]) << 8) |
               static_cast<uint32_t>(block[4 * i + 3]);
    }
    for (unsigned i = 16; i < 64; ++i) {
        const uint32_t s0 = Ror32(w[i - 15], 7) ^ Ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = Ror32(w[i - 2], 17) ^ Ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (unsigned i = 0; i < 64; ++i) {
        const uint32_t s1 = Ror32(e, 6) ^ Ror32(e, 11) ^ Ror32(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = hh + s1 + ch + kSha256K[i] + w[i];
        const uint32_t s0 = Ror32(a, 2) ^ Ror32(a, 13) ^ Ror32(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void Sha256(const uint8_t* in, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const size_t full = len / 64;
    for (size_t i = 0; i < full; ++i) {
        Sha256Compress(h, in + 64 * i);
    }
    uint8_t tail[128] = {};
    const size_t rem = len - full * 64;
    std::memcpy(tail, in + full * 64, rem);
    tail[rem] = 0x80;
    const size_t pad_total = (rem + 9 <= 64) ? 64 : 128;
    const uint64_t bits = static_cast<uint64_t>(len) * 8;
    for (unsigned i = 0; i < 8; ++i) {
        tail[pad_total - 1 - i] = static_cast<uint8_t>(bits >> (8 * i));
    }
    Sha256Compress(h, tail);
    if (pad_total == 128) {
        Sha256Compress(h, tail + 64);
    }
    for (unsigned i = 0; i < 8; ++i) {
        for (unsigned j = 0; j < 4; ++j) {
            out[4 * i + j] = static_cast<uint8_t>(h[i] >> (24 - 8 * j));
        }
    }
}

// ---- MD5 ----

constexpr uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

constexpr unsigned kMd5S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

uint32_t Rol32(uint32_t x, unsigned n) {
    return (x << n) | (x >> (32 - n));
}

void Md5Compress(uint32_t h[4], const uint8_t block[64]) {
    uint32_t m[16];
    for (unsigned i = 0; i < 16; ++i) {
        m[i] = static_cast<uint32_t>(block[4 * i]) |
               (static_cast<uint32_t>(block[4 * i + 1]) << 8) |
               (static_cast<uint32_t>(block[4 * i + 2]) << 16) |
               (static_cast<uint32_t>(block[4 * i + 3]) << 24);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t f = 0;
        unsigned g = 0;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const uint32_t tmp = d;
        d = c;
        c = b;
        b = b + Rol32(a + f + kMd5K[i] + m[g], kMd5S[i]);
        a = tmp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
}

void Md5(const uint8_t* in, size_t len, uint8_t out[16]) {
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const size_t full = len / 64;
    for (size_t i = 0; i < full; ++i) {
        Md5Compress(h, in + 64 * i);
    }
    uint8_t tail[128] = {};
    const size_t rem = len - full * 64;
    std::memcpy(tail, in + full * 64, rem);
    tail[rem] = 0x80;
    const size_t pad_total = (rem + 9 <= 64) ? 64 : 128;
    const uint64_t bits = static_cast<uint64_t>(len) * 8;
    for (unsigned i = 0; i < 8; ++i) {
        tail[pad_total - 8 + i] = static_cast<uint8_t>(bits >> (8 * i));
    }
    Md5Compress(h, tail);
    if (pad_total == 128) {
        Md5Compress(h, tail + 64);
    }
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned j = 0; j < 4; ++j) {
            out[4 * i + j] = static_cast<uint8_t>(h[i] >> (8 * j));
        }
    }
}

// ---- HMAC-SHA256 ----

void HmacSha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len,
                uint8_t out[32]) {
    uint8_t k[64] = {};
    if (key_len > 64) {
        Sha256(key, key_len, k);
    } else {
        std::memcpy(k, key, key_len);
    }
    uint8_t ipad[64];
    uint8_t opad[64];
    for (unsigned i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    std::vector<uint8_t> inner(ipad, ipad + 64);
    inner.insert(inner.end(), msg, msg + msg_len);
    uint8_t inner_hash[32];
    Sha256(inner.data(), inner.size(), inner_hash);
    std::vector<uint8_t> outer(opad, opad + 64);
    outer.insert(outer.end(), inner_hash, inner_hash + 32);
    Sha256(outer.data(), outer.size(), out);
}

// ---- AES-128 ----

constexpr uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

struct InvSboxInit {
    uint8_t table[256];
    InvSboxInit() {
        for (unsigned i = 0; i < 256; ++i) {
            table[kSbox[i]] = static_cast<uint8_t>(i);
        }
    }
};
const InvSboxInit kInvSbox;

constexpr uint8_t kRcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

struct Aes128 {
    uint8_t round_keys[176];  // 11 round keys of 16 bytes

    explicit Aes128(const uint8_t key[16]) {
        uint32_t w[44];
        for (unsigned i = 0; i < 4; ++i) {
            w[i] = (static_cast<uint32_t>(key[4 * i]) << 24) |
                   (static_cast<uint32_t>(key[4 * i + 1]) << 16) |
                   (static_cast<uint32_t>(key[4 * i + 2]) << 8) |
                   static_cast<uint32_t>(key[4 * i + 3]);
        }
        for (unsigned i = 4; i < 44; ++i) {
            uint32_t t = w[i - 1];
            if (i % 4 == 0) {
                const uint32_t rotated = (t << 8) | (t >> 24);
                t = (static_cast<uint32_t>(kSbox[(rotated >> 24) & 0xff]) << 24) |
                    (static_cast<uint32_t>(kSbox[(rotated >> 16) & 0xff]) << 16) |
                    (static_cast<uint32_t>(kSbox[(rotated >> 8) & 0xff]) << 8) |
                    static_cast<uint32_t>(kSbox[rotated & 0xff]);
                t ^= static_cast<uint32_t>(kRcon[i / 4 - 1]) << 24;
            }
            w[i] = w[i - 4] ^ t;
        }
        for (unsigned n = 0; n < 11; ++n) {
            for (unsigned j = 0; j < 16; ++j) {
                round_keys[16 * n + j] =
                    static_cast<uint8_t>(w[4 * n + j / 4] >> (24 - 8 * (j % 4)));
            }
        }
    }

    static uint8_t Xtime(uint8_t x) {
        return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
    }

    static uint8_t GfMul(uint8_t a, uint8_t b) {
        uint8_t out = 0;
        while (b != 0) {
            if (b & 1) {
                out ^= a;
            }
            a = Xtime(a);
            b >>= 1;
        }
        return out;
    }

    void AddRoundKey(uint8_t s[16], unsigned round) const {
        for (unsigned i = 0; i < 16; ++i) {
            s[i] ^= round_keys[16 * round + i];
        }
    }

    static void SubBytes(uint8_t s[16]) {
        for (unsigned i = 0; i < 16; ++i) {
            s[i] = kSbox[s[i]];
        }
    }

    static void InvSubBytes(uint8_t s[16]) {
        for (unsigned i = 0; i < 16; ++i) {
            s[i] = kInvSbox.table[s[i]];
        }
    }

    static void ShiftRows(uint8_t s[16]) {
        uint8_t t[16];
        for (unsigned c = 0; c < 4; ++c) {
            for (unsigned r = 0; r < 4; ++r) {
                t[4 * c + r] = s[4 * ((c + r) % 4) + r];
            }
        }
        std::memcpy(s, t, 16);
    }

    static void InvShiftRows(uint8_t s[16]) {
        uint8_t t[16];
        for (unsigned c = 0; c < 4; ++c) {
            for (unsigned r = 0; r < 4; ++r) {
                t[4 * c + r] = s[4 * ((c + 4 - r) % 4) + r];
            }
        }
        std::memcpy(s, t, 16);
    }

    static void MixColumns(uint8_t s[16]) {
        for (unsigned c = 0; c < 4; ++c) {
            uint8_t* col = s + 4 * c;
            const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
            col[0] = Xtime(a0) ^ (Xtime(a1) ^ a1) ^ a2 ^ a3;
            col[1] = a0 ^ Xtime(a1) ^ (Xtime(a2) ^ a2) ^ a3;
            col[2] = a0 ^ a1 ^ Xtime(a2) ^ (Xtime(a3) ^ a3);
            col[3] = (Xtime(a0) ^ a0) ^ a1 ^ a2 ^ Xtime(a3);
        }
    }

    static void InvMixColumns(uint8_t s[16]) {
        for (unsigned c = 0; c < 4; ++c) {
            uint8_t* col = s + 4 * c;
            const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
            col[0] = GfMul(a0, 14) ^ GfMul(a1, 11) ^ GfMul(a2, 13) ^ GfMul(a3, 9);
            col[1] = GfMul(a0, 9) ^ GfMul(a1, 14) ^ GfMul(a2, 11) ^ GfMul(a3, 13);
            col[2] = GfMul(a0, 13) ^ GfMul(a1, 9) ^ GfMul(a2, 14) ^ GfMul(a3, 11);
            col[3] = GfMul(a0, 11) ^ GfMul(a1, 13) ^ GfMul(a2, 9) ^ GfMul(a3, 14);
        }
    }

    void EncryptBlock(const uint8_t in[16], uint8_t out[16]) const {
        uint8_t s[16];
        std::memcpy(s, in, 16);
        AddRoundKey(s, 0);
        for (unsigned round = 1; round < 10; ++round) {
            SubBytes(s);
            ShiftRows(s);
            MixColumns(s);
            AddRoundKey(s, round);
        }
        SubBytes(s);
        ShiftRows(s);
        AddRoundKey(s, 10);
        std::memcpy(out, s, 16);
    }

    void DecryptBlock(const uint8_t in[16], uint8_t out[16]) const {
        uint8_t s[16];
        std::memcpy(s, in, 16);
        AddRoundKey(s, 10);
        for (unsigned round = 9; round >= 1; --round) {
            InvShiftRows(s);
            InvSubBytes(s);
            AddRoundKey(s, round);
            InvMixColumns(s);
        }
        InvShiftRows(s);
        InvSubBytes(s);
        AddRoundKey(s, 0);
        std::memcpy(out, s, 16);
    }
};

// ---- GCM (NIST SP 800-38D; 96-bit IV, 128-bit tag only) ----

void Gf128Mult(const uint8_t x[16], const uint8_t y[16], uint8_t out[16]) {
    uint8_t z[16] = {};
    uint8_t v[16];
    std::memcpy(v, y, 16);
    for (unsigned i = 0; i < 128; ++i) {
        if (x[i / 8] & (0x80u >> (i % 8))) {
            for (unsigned j = 0; j < 16; ++j) {
                z[j] ^= v[j];
            }
        }
        const unsigned lsb = v[15] & 0x01;
        for (int j = 15; j > 0; --j) {
            v[j] = static_cast<uint8_t>((v[j] >> 1) | ((v[j - 1] & 0x01) << 7));
        }
        v[0] >>= 1;
        if (lsb != 0) {
            v[0] ^= 0xe1;
        }
    }
    std::memcpy(out, z, 16);
}

void Ghash(const uint8_t h[16], const uint8_t* aad, size_t aad_len, const uint8_t* ct,
           size_t ct_len, uint8_t out[16]) {
    uint8_t y[16] = {};
    auto absorb = [&](const uint8_t* data, size_t len) {
        for (size_t off = 0; off < len; off += 16) {
            uint8_t block[16] = {};
            const size_t n = (len - off < 16) ? len - off : 16;
            std::memcpy(block, data + off, n);
            for (unsigned j = 0; j < 16; ++j) {
                y[j] ^= block[j];
            }
            Gf128Mult(y, h, y);
        }
    };
    absorb(aad, aad_len);
    absorb(ct, ct_len);
    uint8_t len_block[16] = {};
    const uint64_t aad_bits = static_cast<uint64_t>(aad_len) * 8;
    const uint64_t ct_bits = static_cast<uint64_t>(ct_len) * 8;
    for (unsigned i = 0; i < 8; ++i) {
        len_block[7 - i] = static_cast<uint8_t>(aad_bits >> (8 * i));
        len_block[15 - i] = static_cast<uint8_t>(ct_bits >> (8 * i));
    }
    for (unsigned j = 0; j < 16; ++j) {
        y[j] ^= len_block[j];
    }
    Gf128Mult(y, h, out);
}

void Inc32(uint8_t block[16]) {
    for (int i = 15; i >= 12; --i) {
        if (++block[i] != 0) {
            break;
        }
    }
}

// ---- Key slots ----

constexpr size_t kMaxKeys = 64;

struct KeySlot {
    bool used = false;
    uint8_t data[64] = {};
    size_t data_len = 0;
    psa_key_type_t type = 0;
    psa_algorithm_t alg = 0;
};

KeySlot g_slots[kMaxKeys];

KeySlot* FindSlot(mbedtls_svc_key_id_t key) {
    if (key == 0 || key > kMaxKeys) {
        return nullptr;
    }
    KeySlot* slot = &g_slots[key - 1];
    return slot->used ? slot : nullptr;
}

}  // namespace

extern "C" {

void psa_set_key_type(psa_key_attributes_t* attributes, psa_key_type_t type) {
    attributes->type = type;
}

void psa_set_key_bits(psa_key_attributes_t* attributes, psa_key_bits_t bits) {
    attributes->bits = bits;
}

void psa_set_key_usage_flags(psa_key_attributes_t* attributes, psa_key_usage_t usage) {
    attributes->usage = usage;
}

void psa_set_key_algorithm(psa_key_attributes_t* attributes, psa_algorithm_t alg) {
    attributes->alg = alg;
}

psa_status_t psa_crypto_init(void) {
    return PSA_SUCCESS;
}

psa_status_t psa_import_key(const psa_key_attributes_t* attributes, const uint8_t* data,
                            size_t data_length, mbedtls_svc_key_id_t* key) {
    if (attributes == nullptr || data == nullptr || key == nullptr || data_length == 0 ||
        data_length > sizeof(g_slots[0].data)) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    if (attributes->type == PSA_KEY_TYPE_AES && attributes->bits != 0 &&
        attributes->bits / 8 != data_length) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < kMaxKeys; ++i) {
        if (!g_slots[i].used) {
            g_slots[i].used = true;
            std::memcpy(g_slots[i].data, data, data_length);
            g_slots[i].data_len = data_length;
            g_slots[i].type = attributes->type;
            g_slots[i].alg = attributes->alg;
            *key = static_cast<mbedtls_svc_key_id_t>(i + 1);
            return PSA_SUCCESS;
        }
    }
    return PSA_ERROR_INSUFFICIENT_MEMORY;
}

psa_status_t psa_destroy_key(mbedtls_svc_key_id_t key) {
    if (key == 0 || key > kMaxKeys) {
        return PSA_ERROR_INVALID_HANDLE;
    }
    g_slots[key - 1].used = false;
    return PSA_SUCCESS;
}

psa_status_t psa_hash_compute(psa_algorithm_t alg, const uint8_t* input, size_t input_length,
                              uint8_t* hash, size_t hash_size, size_t* hash_length) {
    if (input == nullptr && input_length != 0) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    if (alg == PSA_ALG_SHA_256) {
        if (hash_size < 32 || hash == nullptr || hash_length == nullptr) {
            return PSA_ERROR_BUFFER_TOO_SMALL;
        }
        Sha256(input, input_length, hash);
        *hash_length = 32;
        return PSA_SUCCESS;
    }
    if (alg == PSA_ALG_MD5) {
        if (hash_size < 16 || hash == nullptr || hash_length == nullptr) {
            return PSA_ERROR_BUFFER_TOO_SMALL;
        }
        Md5(input, input_length, hash);
        *hash_length = 16;
        return PSA_SUCCESS;
    }
    return PSA_ERROR_NOT_SUPPORTED;
}

psa_status_t psa_mac_compute(mbedtls_svc_key_id_t key, psa_algorithm_t alg, const uint8_t* input,
                             size_t input_length, uint8_t* mac, size_t mac_size,
                             size_t* mac_length) {
    const KeySlot* slot = FindSlot(key);
    if (slot == nullptr) {
        return PSA_ERROR_INVALID_HANDLE;
    }
    if ((alg & 0xff000000u) != 0x01000000u || (alg & 0x00ffffffu) != PSA_ALG_SHA_256) {
        return PSA_ERROR_NOT_SUPPORTED;
    }
    if (mac == nullptr || mac_size < 32 || mac_length == nullptr) {
        return PSA_ERROR_BUFFER_TOO_SMALL;
    }
    if (input == nullptr && input_length != 0) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    HmacSha256(slot->data, slot->data_len, input, input_length, mac);
    *mac_length = 32;
    return PSA_SUCCESS;
}

psa_status_t psa_aead_encrypt(mbedtls_svc_key_id_t key, psa_algorithm_t alg, const uint8_t* nonce,
                              size_t nonce_length, const uint8_t* additional_data,
                              size_t additional_data_length, const uint8_t* plaintext,
                              size_t plaintext_length, uint8_t* output, size_t output_size,
                              size_t* output_length) {
    const KeySlot* slot = FindSlot(key);
    if (slot == nullptr) {
        return PSA_ERROR_INVALID_HANDLE;
    }
    if (alg != PSA_ALG_GCM || slot->data_len != 16 || nonce_length != 12) {
        return PSA_ERROR_NOT_SUPPORTED;
    }
    if (output_size < plaintext_length + 16 || output == nullptr || output_length == nullptr) {
        return PSA_ERROR_BUFFER_TOO_SMALL;
    }
    const Aes128 aes(slot->data);
    uint8_t h[16];
    const uint8_t zeros[16] = {};
    aes.EncryptBlock(zeros, h);

    uint8_t j0[16] = {};
    std::memcpy(j0, nonce, 12);
    j0[15] = 0x01;

    for (size_t off = 0; off < plaintext_length; off += 16) {
        uint8_t ctr[16];
        std::memcpy(ctr, j0, 16);
        const unsigned blocks = static_cast<unsigned>(off / 16) + 1;
        for (unsigned b = 0; b < blocks; ++b) {
            Inc32(ctr);
        }
        uint8_t ks[16];
        aes.EncryptBlock(ctr, ks);
        const size_t n = (plaintext_length - off < 16) ? plaintext_length - off : 16;
        for (size_t i = 0; i < n; ++i) {
            output[off + i] = plaintext[off + i] ^ ks[i];
        }
    }

    uint8_t s[16];
    Ghash(h, additional_data, additional_data_length, output, plaintext_length, s);
    uint8_t ej0[16];
    aes.EncryptBlock(j0, ej0);
    for (unsigned i = 0; i < 16; ++i) {
        output[plaintext_length + i] = s[i] ^ ej0[i];
    }
    *output_length = plaintext_length + 16;
    return PSA_SUCCESS;
}

psa_status_t psa_aead_decrypt(mbedtls_svc_key_id_t key, psa_algorithm_t alg, const uint8_t* nonce,
                              size_t nonce_length, const uint8_t* additional_data,
                              size_t additional_data_length, const uint8_t* ciphertext,
                              size_t ciphertext_length, uint8_t* output, size_t output_size,
                              size_t* output_length) {
    const KeySlot* slot = FindSlot(key);
    if (slot == nullptr) {
        return PSA_ERROR_INVALID_HANDLE;
    }
    if (alg != PSA_ALG_GCM || slot->data_len != 16 || nonce_length != 12) {
        return PSA_ERROR_NOT_SUPPORTED;
    }
    if (ciphertext_length < 16) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    const size_t pt_len = ciphertext_length - 16;
    if (output_size < pt_len || output == nullptr || output_length == nullptr) {
        return PSA_ERROR_BUFFER_TOO_SMALL;
    }
    const Aes128 aes(slot->data);
    uint8_t h[16];
    const uint8_t zeros[16] = {};
    aes.EncryptBlock(zeros, h);

    uint8_t j0[16] = {};
    std::memcpy(j0, nonce, 12);
    j0[15] = 0x01;

    for (size_t off = 0; off < pt_len; off += 16) {
        uint8_t ctr[16];
        std::memcpy(ctr, j0, 16);
        const unsigned blocks = static_cast<unsigned>(off / 16) + 1;
        for (unsigned b = 0; b < blocks; ++b) {
            Inc32(ctr);
        }
        uint8_t ks[16];
        aes.EncryptBlock(ctr, ks);
        const size_t n = (pt_len - off < 16) ? pt_len - off : 16;
        for (size_t i = 0; i < n; ++i) {
            output[off + i] = ciphertext[off + i] ^ ks[i];
        }
    }

    uint8_t s[16];
    Ghash(h, additional_data, additional_data_length, ciphertext, pt_len, s);
    uint8_t ej0[16];
    aes.EncryptBlock(j0, ej0);
    uint8_t diff = 0;
    for (unsigned i = 0; i < 16; ++i) {
        diff |= static_cast<uint8_t>(s[i] ^ ej0[i] ^ ciphertext[pt_len + i]);
    }
    if (diff != 0) {
        return PSA_ERROR_INVALID_SIGNATURE;
    }
    *output_length = pt_len;
    return PSA_SUCCESS;
}

}  // extern "C"
