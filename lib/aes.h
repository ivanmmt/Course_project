#ifndef AES_H
#define AES_H

#include <stdint.h>

void aes128_set_key(const uint8_t key[16], uint32_t round_keys[44]);
void aes128_encrypt_block(const uint32_t round_keys[44], const uint8_t in[16], uint8_t out[16]);
void aes128_cbc_encrypt(const uint8_t key[16], const uint8_t iv[16],
                        const uint8_t* in, uint32_t len, uint8_t* out);
void aes128_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                        const uint8_t* in, uint32_t len, uint8_t* out);

#endif
