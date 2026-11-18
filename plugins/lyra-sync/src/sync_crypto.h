// sync_crypto.h - SHA-256, TLS1.3-style HKDF-PSK key schedule, AES-256-GCM.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "sync_common.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint32_t s[8]; uint64_t bits; uint8_t buf[64]; size_t fill; } sha256_ctx;
void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *p, size_t n);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256(const void *p, size_t n, uint8_t out[32]);
void hmac_sha256(const uint8_t *key, size_t klen, const void *msg, size_t n, uint8_t out[32]);
void hkdf_sha256(const uint8_t *salt, size_t slen, const uint8_t *ikm, size_t ilen,
                 const char *info, size_t infolen, uint8_t *out, size_t olen);
int gcm_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                const uint8_t *aad, size_t aad_len,
                const uint8_t *pt, size_t n, uint8_t *ct, uint8_t tag[16]);
int gcm_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                const uint8_t *aad, size_t aad_len,
                const uint8_t *ct, size_t n, uint8_t *pt, const uint8_t tag[16]);
void rand_bytes(uint8_t *p, size_t n);            // never fails (may degrade)
int rand_bytes_strong(uint8_t *p, size_t n);      // 0 ok / -1 no OS entropy: fail closed

#ifdef __cplusplus
}
#endif