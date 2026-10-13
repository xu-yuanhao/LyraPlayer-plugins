// MD5 (RFC 1321) and AES-128 (FIPS-197), written from the specs.
#include "crypto.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <zlib.h>

namespace netease {
namespace crypto {
namespace {

uint32_t RotL(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

// Per-round shift amounts and the sine-derived table.
const uint32_t kMd5S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
const uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

const uint8_t kAesSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
    0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
    0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
    0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
    0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
    0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
    0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
    0xb0, 0x54, 0xbb, 0x16};

const uint8_t kAesInvSbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e,
    0x81, 0xf3, 0xd7, 0xfb, 0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87,
    0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb, 0x54, 0x7b, 0x94, 0x32,
    0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49,
    0x6d, 0x8b, 0xd1, 0x25, 0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16,
    0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92, 0x6c, 0x70, 0x48, 0x50,
    0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05,
    0xb8, 0xb3, 0x45, 0x06, 0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02,
    0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b, 0x3a, 0x91, 0x11, 0x41,
    0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8,
    0x1c, 0x75, 0xdf, 0x6e, 0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89,
    0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b, 0xfc, 0x56, 0x3e, 0x4b,
    0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59,
    0x27, 0x80, 0xec, 0x5f, 0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d,
    0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef, 0xa0, 0xe0, 0x3b, 0x4d,
    0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63,
    0x55, 0x21, 0x0c, 0x7d};

const uint8_t kAesRcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10,
                              0x20, 0x40, 0x80, 0x1b, 0x36};

uint8_t Xtime(uint8_t x) {
    return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}
uint8_t Gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    while (b) {
        if (b & 1) p ^= a;
        a = Xtime(a);
        b >>= 1;
    }
    return p;
}

void AesKeyExpand(const uint8_t key[16], uint8_t rk[11][16]) {
    std::memcpy(rk[0], key, 16);
    for (int i = 1; i <= 10; ++i) {
        uint8_t t[4] = {rk[i - 1][13], rk[i - 1][14], rk[i - 1][15],
                        rk[i - 1][12]};
        for (int k = 0; k < 4; ++k) t[k] = kAesSbox[t[k]];
        t[0] ^= kAesRcon[i - 1];
        for (int k = 0; k < 4; ++k) rk[i][k] = rk[i - 1][k] ^ t[k];
        for (int k = 4; k < 16; ++k) rk[i][k] = rk[i - 1][k] ^ rk[i][k - 4];
    }
}

void AesAddRound(uint8_t s[16], const uint8_t rk[16]) {
    for (int i = 0; i < 16; ++i) s[i] ^= rk[i];
}
void AesSub(uint8_t s[16]) {
    for (int i = 0; i < 16; ++i) s[i] = kAesSbox[s[i]];
}
void AesInvSub(uint8_t s[16]) {
    for (int i = 0; i < 16; ++i) s[i] = kAesInvSbox[s[i]];
}
void AesShift(uint8_t s[16]) {
    // columns: s[4*c+r]; row r shifts left by r.
    uint8_t t[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) t[4 * c + r] = s[4 * ((c + r) % 4) + r];
    std::memcpy(s, t, 16);
}
void AesInvShift(uint8_t s[16]) {
    uint8_t t[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) t[4 * c + r] = s[4 * ((c - r + 4) % 4) + r];
    std::memcpy(s, t, 16);
}
void AesMix(uint8_t s[16]) {
    for (int c = 0; c < 4; ++c) {
        uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2],
                a3 = s[4 * c + 3];
        s[4 * c] = Gmul(a0, 2) ^ Gmul(a1, 3) ^ a2 ^ a3;
        s[4 * c + 1] = a0 ^ Gmul(a1, 2) ^ Gmul(a2, 3) ^ a3;
        s[4 * c + 2] = a0 ^ a1 ^ Gmul(a2, 2) ^ Gmul(a3, 3);
        s[4 * c + 3] = Gmul(a0, 3) ^ a1 ^ a2 ^ Gmul(a3, 2);
    }
}
void AesInvMix(uint8_t s[16]) {
    for (int c = 0; c < 4; ++c) {
        uint8_t a0 = s[4 * c], a1 = s[4 * c + 1], a2 = s[4 * c + 2],
                a3 = s[4 * c + 3];
        s[4 * c] = Gmul(a0, 14) ^ Gmul(a1, 11) ^ Gmul(a2, 13) ^ Gmul(a3, 9);
        s[4 * c + 1] = Gmul(a0, 9) ^ Gmul(a1, 14) ^ Gmul(a2, 11) ^ Gmul(a3, 13);
        s[4 * c + 2] = Gmul(a0, 13) ^ Gmul(a1, 9) ^ Gmul(a2, 14) ^ Gmul(a3, 11);
        s[4 * c + 3] = Gmul(a0, 11) ^ Gmul(a1, 13) ^ Gmul(a2, 9) ^ Gmul(a3, 14);
    }
}

}  // namespace

void Md5(const uint8_t *data, size_t len, uint8_t out[16]) {
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    uint64_t bit_len = static_cast<uint64_t>(len) * 8;
    size_t padded = ((len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> msg(padded, 0);
    if (len) std::memcpy(msg.data(), data, len);
    msg[len] = 0x80;
    for (int i = 0; i < 8; ++i)
        msg[padded - 8 + i] = static_cast<uint8_t>(bit_len >> (8 * i));
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; ++i)
            w[i] = static_cast<uint32_t>(msg[off + 4 * i]) |
                   (static_cast<uint32_t>(msg[off + 4 * i + 1]) << 8) |
                   (static_cast<uint32_t>(msg[off + 4 * i + 2]) << 16) |
                   (static_cast<uint32_t>(msg[off + 4 * i + 3]) << 24);
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) {
                f = (B & C) | (~B & D);
                g = i;
            } else if (i < 32) {
                f = (D & B) | (~D & C);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = B ^ C ^ D;
                g = (3 * i + 5) % 16;
            } else {
                f = C ^ (B | ~D);
                g = (7 * i) % 16;
            }
            f = f + A + kMd5K[i] + w[g];
            A = D;
            D = C;
            C = B;
            B = B + RotL(f, kMd5S[i]);
        }
        a += A;
        b += B;
        c += C;
        d += D;
    }
    uint32_t hs[4] = {a, b, c, d};
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k)
            out[4 * i + k] = static_cast<uint8_t>(hs[i] >> (8 * k));
}

std::string Md5Hex(const std::string &s) {
    uint8_t d[16];
    Md5(s, d);
    char buf[33];
    for (int i = 0; i < 16; ++i) std::snprintf(buf + 2 * i, 3, "%02x", d[i]);
    return std::string(buf, 32);
}

void Aes128EcbBlock(const uint8_t key[16], const uint8_t in[16],
                    uint8_t out[16], bool encrypt) {
    uint8_t rk[11][16];
    AesKeyExpand(key, rk);
    uint8_t s[16];
    std::memcpy(s, in, 16);
    if (encrypt) {
        AesAddRound(s, rk[0]);
        for (int r = 1; r <= 9; ++r) {
            AesSub(s);
            AesShift(s);
            AesMix(s);
            AesAddRound(s, rk[r]);
        }
        AesSub(s);
        AesShift(s);
        AesAddRound(s, rk[10]);
    } else {
        AesAddRound(s, rk[10]);
        for (int r = 9; r >= 1; --r) {
            AesInvShift(s);
            AesInvSub(s);
            AesAddRound(s, rk[r]);
            AesInvMix(s);
        }
        AesInvShift(s);
        AesInvSub(s);
        AesAddRound(s, rk[0]);
    }
    std::memcpy(out, s, 16);
}

bool Aes128Ecb(const uint8_t key[16], const uint8_t *in, size_t len,
               std::vector<uint8_t> &out, bool encrypt) {
    if (!in && len) return false;
    if (!encrypt && (len == 0 || len % 16 != 0)) return false;
    out.clear();
    out.reserve(encrypt ? len + 16 : len);
    if (encrypt) {
        size_t pad = 16 - (len % 16);
        if (pad == 0) pad = 16;
        size_t total = len + pad;
        for (size_t off = 0; off < total; off += 16) {
            uint8_t blk[16], enc[16];
            for (int k = 0; k < 16; ++k) {
                size_t at = off + k;
                blk[k] = (at < len) ? in[at] : static_cast<uint8_t>(pad);
            }
            Aes128EcbBlock(key, blk, enc, true);
            out.insert(out.end(), enc, enc + 16);
        }
        return true;
    }
    for (size_t off = 0; off < len; off += 16) {
        uint8_t dec[16];
        Aes128EcbBlock(key, in + off, dec, false);
        out.insert(out.end(), dec, dec + 16);
    }
    uint8_t pad = out.back();
    if (pad == 0 || pad > 16) return false;
    for (size_t i = 0; i < pad; ++i)
        if (out[out.size() - 1 - i] != pad) return false;
    out.resize(out.size() - pad);
    return true;
}

std::string Base64Url(const uint8_t *data, size_t len) {
    static const char *kB64 =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        size_t rem = len - i;
        if (rem > 1) v |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (rem > 2) v |= data[i + 2];
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(rem > 1 ? kB64[(v >> 6) & 63] : '=');
        out.push_back(rem > 2 ? kB64[v & 63] : '=');
    }
    for (char &c : out) {
        if (c == '+') c = '-';
        if (c == '/') c = '_';
    }
    return out;
}

}  // namespace crypto

static const uint8_t kEapiKey[16] = {'e', '8', '2', 'c', 'k', 'e', 'n', 'h',
                                    '8', 'd', 'i', 'c', 'h', 'e', 'n', '8'};

std::string EapiParams(const std::string &api_path,
                       const std::string &body_json) {
    std::string digest = crypto::Md5Hex("nobody" + api_path + "use" +
                                        body_json + "md5forencrypt");
    std::string text = api_path + "-36cd479b6b5-" + body_json +
                       "-36cd479b6b5-" + digest;
    std::vector<uint8_t> enc;
    if (!crypto::Aes128Ecb(kEapiKey,
                           reinterpret_cast<const uint8_t *>(text.data()),
                           text.size(), enc, true))
        return std::string();
    static const char *kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(enc.size() * 2);
    for (uint8_t b : enc) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 15]);
    }
    return out;
}

std::string EapiDecode(const uint8_t *data, size_t len) {
    // Skip leading whitespace like the reference client does.
    size_t i = 0;
    while (i < len && (data[i] == ' ' || data[i] == '\t' || data[i] == '\n' ||
                       data[i] == '\r'))
        ++i;
    if (i < len && (data[i] == '{' || data[i] == '['))
        return std::string(reinterpret_cast<const char *>(data + i),
                           len - i);
    std::vector<uint8_t> dec;
    if (!crypto::Aes128Ecb(kEapiKey, data + i, len - i, dec, false))
        return std::string();
    if (dec.size() >= 2 && dec[0] == 0x1f && dec[1] == 0x8b) {
        // gzip member: inflate with automatic header detection.
        z_stream zs{};
        zs.next_in = dec.data();
        zs.avail_in = static_cast<uInt>(dec.size());
        if (inflateInit2(&zs, 15 + 32) != Z_OK) return std::string();
        std::string out;
        uint8_t chunk[32768];
        int rc = Z_OK;
        while (rc == Z_OK) {
            zs.next_out = chunk;
            zs.avail_out = sizeof(chunk);
            rc = inflate(&zs, Z_NO_FLUSH);
            if (rc != Z_OK && rc != Z_STREAM_END) {
                inflateEnd(&zs);
                return std::string();
            }
            out.append(reinterpret_cast<char *>(chunk),
                       sizeof(chunk) - zs.avail_out);
        }
        inflateEnd(&zs);
        return out;
    }
    return std::string(reinterpret_cast<const char *>(dec.data()), dec.size());
}

std::string CoverLink(const std::string &pic_id) {
    if (pic_id.empty()) return std::string();
    for (char c : pic_id) {
        if (c < '0' || c > '9') return std::string();
    }
    // XOR obfuscation, then md5, then url-safe base64 (service recipe).
    static const char *kXor = "3go8&$8*3*3h0k(2)2";
    size_t xn = std::strlen(kXor);
    std::string xored;
    xored.reserve(pic_id.size());
    for (size_t k = 0; k < pic_id.size(); ++k)
        xored.push_back(
            static_cast<char>(pic_id[k] ^ kXor[k % xn]));
    uint8_t digest[16];
    crypto::Md5(xored, digest);
    std::string enc = crypto::Base64Url(digest, 16);
    int seed = pic_id.back() - '0';
    int p = (seed < 3) ? 1 : (seed < 5) ? 2 : (seed < 8) ? 3 : 4;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "http://p%d.music.126.net/%s/%s.jpg", p,
                  enc.c_str(), pic_id.c_str());
    return std::string(buf);
}

std::string EapiHeaderJson(const std::string &device_id,
                           const std::string &request_id) {
    // Anonymous desktop-profile header (field order matches the service).
    char buf[1024];
    std::snprintf(
        buf, sizeof(buf),
        "{\"osver\":\"Microsoft-Windows-10-Professional-build-22631-64bit\","
        "\"deviceId\":\"%s\",\"os\":\"pc\",\"appver\":\"3.0.18.203152\","
        "\"versioncode\":\"6006066\",\"mobilename\":\"Mi+A3\","
        "\"buildver\":\"1768990079\",\"resolution\":\"2268x1080\","
        "\"__csrf\":\"40ab38f0a305fc4c7ff68e636bcf34aa\",\"channel\":"
        "\"netease\",\"requestId\":\"%s\"}",
        device_id.c_str(), request_id.c_str());
    return std::string(buf);
}

std::string RandomId(size_t n) {
    static const char *kChars =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    // xorshift64* seeded from time+pid: no OS rng dependency.
    static uint64_t state = 0;
    if (!state) {
        uint64_t t = static_cast<uint64_t>(time(nullptr));
        state = (t ^ (static_cast<uint64_t>(getpid()) << 32)) | 1u;
    }
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        out.push_back(kChars[(state * 2685821657736338717ull >> 32) % 62]);
    }
    return out;
}

}  // namespace netease
