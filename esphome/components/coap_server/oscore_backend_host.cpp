#include "oscore_backend.h"

#if defined(USE_COAP_OSCORE) && defined(USE_HOST)

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome::coap_server {

ESPHOME_LOG_TAG(TAG, "coap_server");

bool oscore_hkdf_sha256(const uint8_t *secret, size_t secret_len, const uint8_t *salt, size_t salt_len,
                        const uint8_t *info, size_t info_len, uint8_t *okm, size_t okm_len) {
  EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
  if (ctx == nullptr)
    return false;
  size_t len = okm_len;
  // No salt means a salt of zeros (RFC 5869), which is also OpenSSL's default
  bool ok = EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) > 0 &&
            (salt_len == 0 || EVP_PKEY_CTX_set1_hkdf_salt(ctx, salt, (int) salt_len) > 0) &&
            EVP_PKEY_CTX_set1_hkdf_key(ctx, secret, (int) secret_len) > 0 &&
            EVP_PKEY_CTX_add1_hkdf_info(ctx, info, (int) info_len) > 0 && EVP_PKEY_derive(ctx, okm, &len) > 0 &&
            len == okm_len;
  EVP_PKEY_CTX_free(ctx);
  return ok;
}

bool oscore_aes_ccm_encrypt(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                            const uint8_t *plaintext, size_t plaintext_len, uint8_t *out, size_t out_size,
                            size_t *out_len) {
  if (out_size < plaintext_len + OSCORE_AES_TAG_LEN)
    return false;
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (ctx == nullptr)
    return false;
  int len = 0;
  // CCM needs the plaintext length before the AAD
  bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ccm(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, OSCORE_AES_NONCE_LEN, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, OSCORE_AES_TAG_LEN, nullptr) == 1 &&
            EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1 &&
            EVP_EncryptUpdate(ctx, nullptr, &len, nullptr, (int) plaintext_len) == 1 &&
            EVP_EncryptUpdate(ctx, nullptr, &len, aad, (int) aad_len) == 1 &&
            EVP_EncryptUpdate(ctx, out, &len, plaintext, (int) plaintext_len) == 1 &&
            EVP_EncryptFinal_ex(ctx, out + len, &len) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, OSCORE_AES_TAG_LEN, out + plaintext_len) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) {
    ESP_LOGE(TAG, "OSCORE: encryption failed");
    return false;
  }
  *out_len = plaintext_len + OSCORE_AES_TAG_LEN;
  return true;
}

bool oscore_aes_ccm_decrypt(const uint8_t *key, const uint8_t *nonce, const uint8_t *aad, size_t aad_len,
                            const uint8_t *in, size_t in_len, uint8_t *out, size_t out_size, size_t *out_len) {
  if (in_len < OSCORE_AES_TAG_LEN || out_size < in_len - OSCORE_AES_TAG_LEN)
    return false;
  const size_t cipher_len = in_len - OSCORE_AES_TAG_LEN;
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (ctx == nullptr)
    return false;
  int len = 0;
  // For CCM the last update checks the tag
  bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_ccm(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, OSCORE_AES_NONCE_LEN, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, OSCORE_AES_TAG_LEN,
                                const_cast<uint8_t *>(in + cipher_len)) == 1 &&
            EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1 &&
            EVP_DecryptUpdate(ctx, nullptr, &len, nullptr, (int) cipher_len) == 1 &&
            EVP_DecryptUpdate(ctx, nullptr, &len, aad, (int) aad_len) == 1 &&
            EVP_DecryptUpdate(ctx, out, &len, in, (int) cipher_len) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) {
    ESP_LOGW(TAG, "OSCORE: decryption failed");
    return false;
  }
  *out_len = cipher_len;
  return true;
}

// <prefs dir>/<name>.coap_oscore, next to the host preferences file
static std::string seq_path() {
  std::string dir;
  if (const char *prefdir = getenv("ESPHOME_PREFDIR"); prefdir != nullptr) {
    dir = prefdir;
  } else if (const char *home = getenv("HOME"); home != nullptr) {
    dir = std::string(home) + "/.esphome/prefs";
  } else {
    return {};
  }
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec)
    return {};
  return dir + "/" + App.get_name().c_str() + ".coap_oscore";
}

bool oscore_seq_load(uint32_t *value) {
  const std::string path = seq_path();
  if (path.empty())
    return false;
  FILE *fp = fopen(path.c_str(), "rb");
  if (fp == nullptr)
    return false;
  bool ok = fread(value, sizeof(*value), 1, fp) == 1;
  fclose(fp);
  return ok;
}

// Written to a temporary file, synced, then renamed, so a crash leaves the old value or the new one
bool oscore_seq_save(uint32_t value) {
  const std::string path = seq_path();
  if (path.empty()) {
    ESP_LOGE(TAG, "OSCORE: ESPHOME_PREFDIR and HOME are not set, cannot store the sequence number");
    return false;
  }
  const std::string tmp = path + ".tmp";
  FILE *fp = fopen(tmp.c_str(), "wb");
  if (fp == nullptr)
    return false;
  bool ok = fwrite(&value, sizeof(value), 1, fp) == 1 && fflush(fp) == 0 && fsync(fileno(fp)) == 0;
  ok = fclose(fp) == 0 && ok;
  return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace esphome::coap_server

#endif  // USE_COAP_OSCORE && USE_HOST
