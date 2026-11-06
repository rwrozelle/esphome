#include "oscore_backend.h"

#if defined(USE_COAP_OSCORE) && defined(USE_ESP32)

#include <nvs.h>
#include <psa/crypto.h>
#include "esphome/core/log.h"

namespace esphome::coap_server {

ESPHOME_LOG_TAG(TAG, "coap_server");

static constexpr const char *SEQ_NAMESPACE = "coap_oscore";
static constexpr const char *SEQ_KEY = "seq";
static constexpr psa_algorithm_t CCM_ALG = PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, OSCORE_AES_TAG_LEN);

bool oscore_hkdf_sha256(const uint8_t *secret, size_t secret_len, const uint8_t *salt, size_t salt_len,
                        const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len) {
  psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
  psa_status_t s = psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256));
  if (s == PSA_SUCCESS && salt_len > 0)
    s = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len);
  if (s == PSA_SUCCESS)
    s = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET, secret, secret_len);
  if (s == PSA_SUCCESS)
    s = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, info, info_len);
  if (s == PSA_SUCCESS)
    s = psa_key_derivation_output_bytes(&op, okm, okm_len);
  psa_key_derivation_abort(&op);
  return s == PSA_SUCCESS;
}

// Imports key as a transient PSA key for one operation; the caller destroys it
static bool import_key(const uint8_t *key, psa_key_usage_t usage, psa_key_id_t *key_id) {
  psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attrs, OSCORE_AES_KEY_LEN * 8);
  psa_set_key_usage_flags(&attrs, usage);
  psa_set_key_algorithm(&attrs, CCM_ALG);
  psa_status_t s = psa_import_key(&attrs, key, OSCORE_AES_KEY_LEN, key_id);
  if (s != PSA_SUCCESS) {
    ESP_LOGE(TAG, "OSCORE: key import failed: %d", (int) s);
    return false;
  }
  return true;
}

bool oscore_aes_ccm_encrypt(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                            const uint8_t *plaintext, size_t plaintext_len, uint8_t *out, size_t out_size,
                            size_t *out_len) {
  psa_key_id_t key_id = PSA_KEY_ID_NULL;
  if (!import_key(key, PSA_KEY_USAGE_ENCRYPT, &key_id))
    return false;
  psa_status_t s = psa_aead_encrypt(key_id, CCM_ALG, nonce, OSCORE_AES_NONCE_LEN, aad, aad_len, plaintext,
                                    plaintext_len, out, out_size, out_len);
  psa_destroy_key(key_id);
  if (s != PSA_SUCCESS) {
    ESP_LOGE(TAG, "OSCORE: encryption failed: %d", (int) s);
    return false;
  }
  return true;
}

bool oscore_aes_ccm_decrypt(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                            const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size, size_t *out_len) {
  psa_key_id_t key_id = PSA_KEY_ID_NULL;
  if (!import_key(key, PSA_KEY_USAGE_DECRYPT, &key_id))
    return false;
  psa_status_t s =
      psa_aead_decrypt(key_id, CCM_ALG, nonce, OSCORE_AES_NONCE_LEN, aad, aad_len, in, in_len, out, out_size, out_len);
  psa_destroy_key(key_id);
  if (s != PSA_SUCCESS) {
    ESP_LOGW(TAG, "OSCORE: decryption failed: %d", (int) s);
    return false;
  }
  return true;
}

bool oscore_seq_load(uint32_t *value) {
  nvs_handle_t handle;
  if (nvs_open(SEQ_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    return false;
  esp_err_t err = nvs_get_u32(handle, SEQ_KEY, value);
  nvs_close(handle);
  return err == ESP_OK;
}

// Written straight to NVS: preferences only reach flash on their periodic sync, which a user may suspend
bool oscore_seq_save(uint32_t value) {
  nvs_handle_t handle;
  if (nvs_open(SEQ_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    return false;
  bool ok = nvs_set_u32(handle, SEQ_KEY, value) == ESP_OK && nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  return ok;
}

}  // namespace esphome::coap_server

#endif  // USE_COAP_OSCORE && USE_ESP32
