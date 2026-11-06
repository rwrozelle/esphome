#pragma once

#include "esphome/core/defines.h"

#ifdef USE_COAP_OSCORE

#include <cstddef>
#include <cstdint>

// Platform part of OSCORE: the crypto primitives and the persisted sender sequence number.
namespace esphome::coap_server {

// AES-CCM-16-64-128 (COSE algorithm 10)
static constexpr size_t OSCORE_AES_KEY_LEN = 16;
static constexpr size_t OSCORE_AES_NONCE_LEN = 13;
static constexpr size_t OSCORE_AES_TAG_LEN = 8;

bool oscore_hkdf_sha256(const uint8_t *secret, size_t secret_len, const uint8_t *salt, size_t salt_len,
                        const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len);

// out receives the ciphertext followed by the tag
bool oscore_aes_ccm_encrypt(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                            const uint8_t *plaintext, size_t plaintext_len, uint8_t *out, size_t out_size,
                            size_t *out_len);
// in is the ciphertext followed by the tag; false when the tag does not match
bool oscore_aes_ccm_decrypt(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                            const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size, size_t *out_len);

// The value must be on permanent storage when save returns true. load returns false when nothing is stored.
bool oscore_seq_load(uint32_t *value);
bool oscore_seq_save(uint32_t value);

}  // namespace esphome::coap_server

#endif  // USE_COAP_OSCORE
