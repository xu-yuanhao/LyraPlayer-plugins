#pragma once
// Small crypto primitives for the eapi transport, written from the public
// algorithm specs (RFC 1321 MD5, FIPS-197 AES). No third-party code.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace netease {
namespace crypto {

// 16-byte MD5 digest of data.
void Md5(const uint8_t *data, size_t len, uint8_t out[16]);
inline void Md5(const std::string &s, uint8_t out[16]) {
    Md5(reinterpret_cast<const uint8_t *>(s.data()), s.size(), out);
}
std::string Md5Hex(const std::string &s);

// AES-128-ECB single block (16 bytes in/out).
void Aes128EcbBlock(const uint8_t key[16], const uint8_t in[16],
                    uint8_t out[16], bool encrypt);

// PKCS#7 padded ECB encrypt/decrypt of arbitrary length.
// Returns false on bad length (decrypt) only; encrypt always succeeds.
bool Aes128Ecb(const uint8_t key[16], const uint8_t *in, size_t len,
               std::vector<uint8_t> &out, bool encrypt);

// Standard base64 (with +/) then made url-safe like the cover scheme.
std::string Base64Url(const uint8_t *data, size_t len);

}  // namespace crypto

// ---- Netease protocol helpers (facts reimplemented, no copied code) ----

// eapi request envelope: params = HEX.UPPER(AES_ECB(api-url + magic + json +
// magic + md5, eapiKey)). body_json must already contain header/e_r.
std::string EapiParams(const std::string &api_path, const std::string &body_json);

// eapi response: plain JSON passes through, otherwise AES_ECB decrypt.
// Returns the JSON text, or "" on failure.
std::string EapiDecode(const uint8_t *data, size_t len);

// cover id -> "http://pN.music.126.net/<enc>/<id>.jpg" (caller upgrades).
std::string CoverLink(const std::string &pic_id);

// Minimal anonymous client header object (field order matches the service).
std::string EapiHeaderJson(const std::string &device_id,
                           const std::string &request_id);

std::string RandomId(size_t n);  // [0-9a-zA-Z]

}  // namespace netease
