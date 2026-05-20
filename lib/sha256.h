#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>

#define SHA256_BLOCK_SIZE 64
#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  data[SHA256_BLOCK_SIZE];
    uint32_t datalen;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t* ctx);
void sha256_update(sha256_ctx_t* ctx, const uint8_t* data, uint32_t len);
void sha256_final(sha256_ctx_t* ctx, uint8_t digest[SHA256_DIGEST_SIZE]);
void sha256(const uint8_t* data, uint32_t len, uint8_t digest[SHA256_DIGEST_SIZE]);
void hmac_sha256(const uint8_t* key, uint32_t key_len,
                 const uint8_t* data, uint32_t data_len,
                 uint8_t out[SHA256_DIGEST_SIZE]);

#endif
