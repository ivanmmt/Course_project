#ifndef RSA_H
#define RSA_H

#include <stdint.h>
#include <stdbool.h>

#define RSA_MAX_BYTES 512

typedef struct {
    uint8_t n[RSA_MAX_BYTES];
    uint32_t n_len;
    uint8_t e[RSA_MAX_BYTES];
    uint32_t e_len;
} rsa_pubkey_t;

bool rsa_parse_pubkey_from_cert(const uint8_t* der, uint32_t der_len, rsa_pubkey_t* out);
bool rsa_pkcs1_v15_encrypt(const rsa_pubkey_t* pk, const uint8_t* msg, uint32_t msg_len,
                           uint8_t* out, uint32_t* out_len);

#endif
