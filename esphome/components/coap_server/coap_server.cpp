#include "coap_server.h"
#include "esphome/core/application.h"
#include "esphome/core/device.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "cbor.h"
#ifdef USE_LOGGER
#include "esphome/components/logger/logger.h"
#endif

namespace esphome::coap_server {

ESPHOME_LOG_TAG(TAG, "coap_server");

CoapServer *global_coap_server = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
CoapServer::CoapServer() { global_coap_server = this; }

void CoapServer::setup() {
#ifdef USE_COAP_OSCORE
  if (!this->oscore_derive_keys_()) {
    this->mark_failed(LOG_STR("OSCORE key derivation failed"));
    return;
  }
  this->oscore_restore_seq_no_();
#endif  // USE_COAP_OSCORE

  {
    size_t size = build_link_format(nullptr, 0);
    this->link_format_buf_ = std::make_unique<uint8_t[]>(size);
    build_link_format(this->link_format_buf_.get(), size);
    this->link_format_size_ = size;
  }
}

#ifdef USE_BINARY_SENSOR
void CoapServer::on_binary_sensor_update(binary_sensor::BinarySensor *entity) { this->on_entity_update(entity); }
#endif
#ifdef USE_LOCK
void CoapServer::on_lock_update(lock::Lock *entity) { this->on_entity_update(entity); }
#endif
#ifdef USE_NUMBER
void CoapServer::on_number_update(number::Number *entity) { this->on_entity_update(entity); }
#endif
#ifdef USE_SENSOR
void CoapServer::on_sensor_update(sensor::Sensor *entity) { this->on_entity_update(entity); }
#endif
#ifdef USE_SWITCH
void CoapServer::on_switch_update(switch_::Switch *entity) { this->on_entity_update(entity); }
#endif
#ifdef USE_TEXT_SENSOR
void CoapServer::on_text_sensor_update(text_sensor::TextSensor *entity) { this->on_entity_update(entity); }
#endif
#ifdef USE_VALVE
void CoapServer::on_valve_update(valve::Valve *entity) { this->on_entity_update(entity); }
#endif

// static
bool CoapServer::blockwise_transmit_hook(void *ctx, uint8_t *block_buf, uint32_t position, uint16_t *block_length,
                                         bool *more) {
  auto *src = static_cast<BlockwiseSource *>(ctx);
  if (src->read_fn != nullptr)
    return src->read_fn(src->read_ctx, block_buf, position, block_length, more);
  if (position >= src->data_len)
    return false;
  size_t avail = src->data_len - position;
  uint16_t len = (uint16_t) std::min((size_t) *block_length, avail);
  memcpy(block_buf, src->data + position, len);
  *block_length = len;
  *more = (position + len) < src->data_len;
  return true;
}

// static
size_t CoapServer::build_link_format(uint8_t *buf, size_t buf_len) {
  char entry[LINK_ENTRY_MAX_SIZE];
  char path[32];
  size_t total = 0;
  uint16_t senml = 1;

#ifdef USE_DEVICES
  auto get_device_idx = [](EntityBase *e) -> uint8_t {
    if (e == nullptr || e->get_device_id() == 0)
      return 0;
    uint8_t dev_idx = 1;
    for (Device *dev : App.get_devices()) {
      if (dev->get_device_id() == e->get_device_id())
        return dev_idx;
      dev_idx++;
    }
    return 0;
  };
#else
  auto get_device_idx = [](EntityBase *) -> uint8_t { return 0; };
#endif

  auto make_path = [&](uint16_t idx, char suffix) {
    path[0] = 'f';
    path[1] = 'p';
    path[2] = '/';
    uint8_t p = 3;
    append_uint16_decimal(path, p, idx);
    path[p++] = '/';
    path[p++] = 'g';
    path[p++] = '/';
    path[p++] = suffix;
    path[p] = '\0';
  };

  auto emit = [&](const LinkFormatResource &v, bool comma) {
    uint16_t len = format_link_entry(entry, sizeof(entry), v, comma);
    if (buf != nullptr && total + len <= buf_len)
      memcpy(buf + total, entry, len);
    total += len;
  };

  // /info — first entry, no leading comma
  emit({"info", "device", nullptr, ENTITYTYPE_DEVICE, ACTIONTYPE_NO_ACTION, false, 0}, false);

#ifdef USE_BINARY_SENSOR
  for (auto *e : App.get_binary_sensors()) {
    if (e->is_internal())
      continue;
    make_path(senml++, '1');
    emit({path, entity_type_domain_name(ENTITYTYPE_BINARY_SENSOR), e, ENTITYTYPE_BINARY_SENSOR, ACTIONTYPE_NO_ACTION,
          true, get_device_idx(e)},
         true);
  }
#endif
#ifdef USE_BUTTON
  for (auto *e : App.get_buttons()) {
    if (e->is_internal())
      continue;
    make_path(senml++, '1');
    emit({path, entity_type_domain_name(ENTITYTYPE_BUTTON), e, ENTITYTYPE_BUTTON, ACTIONTYPE_NO_ACTION, false,
          get_device_idx(e)},
         true);
  }
#endif
#ifdef USE_LOCK
  for (auto *e : App.get_locks()) {
    if (e->is_internal())
      continue;
    make_path(senml++, '1');
    emit({path, entity_type_domain_name(ENTITYTYPE_LOCK), e, ENTITYTYPE_LOCK, ACTIONTYPE_NO_ACTION, true,
          get_device_idx(e)},
         true);
  }
#endif
#ifdef USE_NUMBER
  for (auto *e : App.get_numbers()) {
    if (e->is_internal())
      continue;
    make_path(senml++, '1');
    emit({path, entity_type_domain_name(ENTITYTYPE_NUMBER), e, ENTITYTYPE_NUMBER, ACTIONTYPE_NO_ACTION, true,
          get_device_idx(e)},
         true);
  }
#endif
#ifdef USE_SENSOR
  for (auto *e : App.get_sensors()) {
    if (e->is_internal())
      continue;
    make_path(senml++, '1');
    emit({path, entity_type_domain_name(ENTITYTYPE_SENSOR), e, ENTITYTYPE_SENSOR, ACTIONTYPE_NO_ACTION, true,
          get_device_idx(e)},
         true);
  }
#endif
#ifdef USE_SWITCH
  for (auto *e : App.get_switches()) {
    if (e->is_internal())
      continue;
    uint16_t idx = senml++;
    uint8_t dv = get_device_idx(e);
    make_path(idx, '1');
    emit({path, entity_type_domain_name(ENTITYTYPE_SWITCH), e, ENTITYTYPE_SWITCH, ACTIONTYPE_NO_ACTION, true, dv},
         true);
    make_path(idx, '2');
    emit({path, entity_type_domain_name(ENTITYTYPE_SWITCH), e, ENTITYTYPE_SWITCH, ACTIONTYPE_TOGGLE, false, dv}, true);
  }
#endif
#ifdef USE_TEXT_SENSOR
  for (auto *e : App.get_text_sensors()) {
    if (e->is_internal())
      continue;
    make_path(senml++, '1');
    emit({path, entity_type_domain_name(ENTITYTYPE_TEXT_SENSOR), e, ENTITYTYPE_TEXT_SENSOR, ACTIONTYPE_NO_ACTION, true,
          get_device_idx(e)},
         true);
  }
#endif
#ifdef USE_VALVE
  for (auto *e : App.get_valves()) {
    if (e->is_internal())
      continue;
    uint16_t idx = senml++;
    uint8_t dv = get_device_idx(e);
    make_path(idx, '1');
    emit({path, entity_type_domain_name(ENTITYTYPE_VALVE), e, ENTITYTYPE_VALVE, ACTIONTYPE_NO_ACTION, true, dv}, true);
    make_path(idx, '2');
    emit({path, entity_type_domain_name(ENTITYTYPE_VALVE), e, ENTITYTYPE_VALVE, ACTIONTYPE_STOP, false, dv}, true);
  }
#endif
#ifdef USE_LOGGER
  {
    make_path(senml++, '1');
    emit({path, entity_type_domain_name(ENTITYTYPE_LOG), nullptr, ENTITYTYPE_LOG, ACTIONTYPE_NO_ACTION, true, 0}, true);
  }
#endif

  static const char PING_ENTRY[] = R"(,</ping>;rt="esphome.ping";if="if.a";ct=112)";
  size_t ping_len = sizeof(PING_ENTRY) - 1;
  if (buf != nullptr && total + ping_len <= buf_len)
    memcpy(buf + total, PING_ENTRY, ping_len);
  total += ping_len;
  return total;
}

void CoapServer::apply_entity_post(EntityBase *entity, EntityType type, ActionType action, const uint8_t *payload,
                                   size_t payload_len) {
  if (payload_len == 0 || payload_len > COAP_PAYLOAD_MAX_SIZE) {
    ESP_LOGW(TAG, "CoAP: POST payload_len %u rejected (max %u)", (unsigned) payload_len,
             (unsigned) COAP_PAYLOAD_MAX_SIZE);
    return;
  }
  CborParser parser;
  CborValue root, map_val;
  if (cbor_parser_init(payload, payload_len, 0, &parser, &root) != CBOR_NO_ERROR || !cbor_value_is_map(&root) ||
      cbor_value_enter_container(&root, &map_val) != CBOR_NO_ERROR)
    return;
  while (!cbor_value_at_end(&map_val)) {
    int key = 0;
    bool key_ok = cbor_value_get_int(&map_val, &key) == CBOR_NO_ERROR;
    // A failed advance does not move the position, so continuing would loop forever
    if (cbor_value_advance(&map_val) != CBOR_NO_ERROR || cbor_value_at_end(&map_val))
      break;
    switch (type) {
#ifdef USE_SWITCH
      case ENTITYTYPE_SWITCH:
        if (key_ok && key == 4 && cbor_value_is_boolean(&map_val)) {
          bool val = false;
          if (cbor_value_get_boolean(&map_val, &val) == CBOR_NO_ERROR) {
            auto *sw = static_cast<switch_::Switch *>(entity);
            if (action == ACTIONTYPE_TOGGLE) {
              if (val) {
                sw->toggle();
              }
            } else {
              if (val) {
                sw->turn_on();
              } else {
                sw->turn_off();
              }
            }
          }
        }
        break;
#endif
#ifdef USE_NUMBER
      case ENTITYTYPE_NUMBER:
        if (key_ok && key == 2) {
          double d = 0.0;
          bool got = false;
          if (cbor_value_is_float(&map_val) || cbor_value_is_double(&map_val) || cbor_value_is_half_float(&map_val)) {
            got = cbor_value_get_double(&map_val, &d) == CBOR_NO_ERROR;
          } else if (cbor_value_is_integer(&map_val)) {
            int64_t i = 0;
            if (cbor_value_get_int64(&map_val, &i) == CBOR_NO_ERROR) {
              d = static_cast<double>(i);
              got = true;
            }
          }
          if (got)
            static_cast<number::Number *>(entity)->make_call().set_value(static_cast<float>(d)).perform();
        }
        break;
#endif
#ifdef USE_LOCK
      case ENTITYTYPE_LOCK:
        if (key_ok && key == 4 && cbor_value_is_boolean(&map_val)) {
          bool val = false;
          if (cbor_value_get_boolean(&map_val, &val) == CBOR_NO_ERROR) {
            auto *lk = static_cast<lock::Lock *>(entity);
            if (val) {
              lk->lock();
            } else {
              lk->unlock();
            }
          }
        }
        break;
#endif
#ifdef USE_VALVE
      case ENTITYTYPE_VALVE:
        if (key_ok && key == 4 && cbor_value_is_boolean(&map_val)) {
          bool val = false;
          if (cbor_value_get_boolean(&map_val, &val) == CBOR_NO_ERROR) {
            auto *vv = static_cast<valve::Valve *>(entity);
            if (action == ACTIONTYPE_STOP) {
              if (val) {
                vv->make_call().set_command_stop().perform();
              }
            } else {
              if (val) {
                vv->make_call().set_command_open().perform();
              } else {
                vv->make_call().set_command_close().perform();
              }
            }
          }
        }
        break;
#endif
      default:
        break;
    }
    if (cbor_value_advance(&map_val) != CBOR_NO_ERROR)
      break;
  }
}

#ifdef USE_COAP_OSCORE

// Encode the OSCORE HKDF info array per RFC 8613 §3.2.1:
// [id (bstr), id_context (bstr / null), alg (int = 10), type (tstr), L (uint)]
static size_t oscore_build_info(uint8_t *buf, size_t buf_len, const uint8_t *id, size_t id_len,
                                const uint8_t *id_context, size_t id_context_len, const char *type, uint8_t key_len) {
  static const uint8_t EMPTY_BUF[1] = {};
  CborEncoder enc, arr;
  cbor_encoder_init(&enc, buf, buf_len, 0);
  if (cbor_encoder_create_array(&enc, &arr, 5) != CBOR_NO_ERROR)
    return 0;
  if (cbor_encode_byte_string(&arr, id_len > 0 ? id : EMPTY_BUF, id_len) != CBOR_NO_ERROR)
    return 0;
  if (id_context_len > 0) {
    if (cbor_encode_byte_string(&arr, id_context, id_context_len) != CBOR_NO_ERROR)
      return 0;
  } else {
    if (cbor_encode_null(&arr) != CBOR_NO_ERROR)
      return 0;
  }
  if (cbor_encode_int(&arr, 10) != CBOR_NO_ERROR)  // AEAD_AES_128_CCM
    return 0;
  if (cbor_encode_text_stringz(&arr, type) != CBOR_NO_ERROR)
    return 0;
  if (cbor_encode_uint(&arr, key_len) != CBOR_NO_ERROR)
    return 0;
  if (cbor_encoder_close_container(&enc, &arr) != CBOR_NO_ERROR)
    return 0;
  return cbor_encoder_get_buffer_size(&enc, buf);
}

// Overwrites key material; volatile stores cannot be dropped as dead before the memory is freed
static void oscore_wipe(void *buf, size_t len) {
  volatile uint8_t *p = static_cast<volatile uint8_t *>(buf);
  for (size_t i = 0; i < len; i++)
    p[i] = 0;
}

static void oscore_wipe(std::vector<uint8_t> &v) {
  if (!v.empty())
    oscore_wipe(v.data(), v.size());
  v.clear();
  v.shrink_to_fit();
}

bool CoapServer::oscore_derive_keys_() {
  const bool ok = this->oscore_derive_keys_from_material_();
  // Only derivation needs the YAML material, so it is wiped whether that worked or not
  oscore_wipe(this->oscore_master_secret_);
  oscore_wipe(this->oscore_master_salt_);
  oscore_wipe(this->oscore_sender_id_);
  oscore_wipe(this->oscore_recipient_id_);
  oscore_wipe(this->oscore_id_context_);
  if (!ok) {
    // Keys from the steps that worked are of no use to a failed component
    oscore_wipe(this->oscore_sender_key_, sizeof(this->oscore_sender_key_));
    oscore_wipe(this->oscore_recipient_key_, sizeof(this->oscore_recipient_key_));
    oscore_wipe(this->oscore_common_iv_, sizeof(this->oscore_common_iv_));
    return false;
  }
  // A new value each boot, so an Echo seen before the reboot cannot start the replay window
  return random_bytes(this->oscore_echo_, sizeof(this->oscore_echo_));
}

bool CoapServer::oscore_derive_keys_from_material_() {
  uint8_t info_buf[64];
  size_t info_len;

  // Sender key — derive and store raw bytes; imported as transient PSA key per encrypt call
  info_len =
      oscore_build_info(info_buf, sizeof(info_buf), this->oscore_sender_id_.data(), this->oscore_sender_id_.size(),
                        this->oscore_id_context_.data(), this->oscore_id_context_.size(), "Key", OSCORE_KEY_LEN);
  if (info_len == 0 || !oscore_hkdf_sha256(this->oscore_master_secret_.data(), this->oscore_master_secret_.size(),
                                           this->oscore_master_salt_.data(), this->oscore_master_salt_.size(), info_buf,
                                           info_len, this->oscore_sender_key_, OSCORE_KEY_LEN)) {
    return false;
  }

  // Recipient key — derive and store raw bytes; imported as transient PSA key per decrypt call
  info_len = oscore_build_info(info_buf, sizeof(info_buf), this->oscore_recipient_id_.data(),
                               this->oscore_recipient_id_.size(), this->oscore_id_context_.data(),
                               this->oscore_id_context_.size(), "Key", OSCORE_KEY_LEN);
  if (info_len == 0 || !oscore_hkdf_sha256(this->oscore_master_secret_.data(), this->oscore_master_secret_.size(),
                                           this->oscore_master_salt_.data(), this->oscore_master_salt_.size(), info_buf,
                                           info_len, this->oscore_recipient_key_, OSCORE_KEY_LEN)) {
    return false;
  }

  // Common IV — stored as raw bytes for nonce construction (not an AEAD key)
  info_len = oscore_build_info(info_buf, sizeof(info_buf), nullptr, 0, this->oscore_id_context_.data(),
                               this->oscore_id_context_.size(), "IV", OSCORE_IV_LEN);
  if (info_len == 0 || !oscore_hkdf_sha256(this->oscore_master_secret_.data(), this->oscore_master_secret_.size(),
                                           this->oscore_master_salt_.data(), this->oscore_master_salt_.size(), info_buf,
                                           info_len, this->oscore_common_iv_, OSCORE_IV_LEN)) {
    return false;
  }

  ESP_LOGI(TAG, "OSCORE keys derived");

  // Retain sender ID (needed for notification OSCORE option); the caller wipes the rest
  this->oscore_sender_id_len_ = (uint8_t) std::min(this->oscore_sender_id_.size(), sizeof(this->oscore_sender_id_buf_));
  if (this->oscore_sender_id_len_ > 0)  // an empty Sender ID is valid, and its data() may be null
    memcpy(this->oscore_sender_id_buf_, this->oscore_sender_id_.data(), this->oscore_sender_id_len_);
  return true;
}

// static
uint8_t CoapServer::oscore_encode_piv(uint32_t seq, uint8_t *piv) {
  uint8_t len = 1;
  while (len < sizeof(seq) && (seq >> (8 * len)) != 0)
    len++;
  for (uint8_t i = 0; i < len; i++)
    piv[i] = (uint8_t) (seq >> (8 * (len - 1 - i)));
  return len;
}

// Nonce = [kid_len(1) | padded_kid(nonce_len-6) | padded_piv(5)] XOR common_iv
void CoapServer::oscore_build_nonce(const uint8_t *piv, uint8_t piv_len, const uint8_t *kid, uint8_t kid_len,
                                    const uint8_t *common_iv, uint8_t nonce[OSCORE_IV_LEN]) {
  uint8_t input[OSCORE_IV_LEN] = {};
  input[0] = kid_len;
  // kid right-aligned in bytes 1..(OSCORE_IV_LEN-6)
  const uint8_t kid_field_len = OSCORE_IV_LEN - 6;
  if (kid_len <= kid_field_len)
    memcpy(input + 1 + (kid_field_len - kid_len), kid, kid_len);
  // piv right-aligned in last 5 bytes
  if (piv_len <= 5)
    memcpy(input + OSCORE_IV_LEN - 5 + (5 - piv_len), piv, piv_len);
  for (uint8_t i = 0; i < OSCORE_IV_LEN; i++)
    nonce[i] = input[i] ^ common_iv[i];
}

// AAD = CBOR(["Encrypt0", h'', CBOR([1, [10], kid_bstr, piv_bstr, h''])])
size_t CoapServer::oscore_build_aad(const uint8_t *kid, uint8_t kid_len, const uint8_t *piv, uint8_t piv_len,
                                    uint8_t *buf, size_t buf_len) {
  // Build inner aad_array first into a temp buffer
  uint8_t aad_array_buf[32];
  CborEncoder enc, arr;
  cbor_encoder_init(&enc, aad_array_buf, sizeof(aad_array_buf), 0);
  if (cbor_encoder_create_array(&enc, &arr, 5) != CBOR_NO_ERROR)
    return 0;
  if (cbor_encode_uint(&arr, 1) != CBOR_NO_ERROR)  // oscore_version
    return 0;
  CborEncoder alg_arr;
  if (cbor_encoder_create_array(&arr, &alg_arr, 1) != CBOR_NO_ERROR)
    return 0;
  if (cbor_encode_int(&alg_arr, 10) != CBOR_NO_ERROR)  // AES-CCM-16-64-128
    return 0;
  if (cbor_encoder_close_container(&arr, &alg_arr) != CBOR_NO_ERROR)
    return 0;
  if (cbor_encode_byte_string(&arr, kid, kid_len) != CBOR_NO_ERROR)
    return 0;
  if (cbor_encode_byte_string(&arr, piv, piv_len) != CBOR_NO_ERROR)
    return 0;
  static const uint8_t EMPTY_BUF[1] = {};
  if (cbor_encode_byte_string(&arr, EMPTY_BUF, 0) != CBOR_NO_ERROR)  // class-I options (empty)
    return 0;
  if (cbor_encoder_close_container(&enc, &arr) != CBOR_NO_ERROR)
    return 0;
  size_t aad_array_len = cbor_encoder_get_buffer_size(&enc, aad_array_buf);

  // Enc_Structure = ["Encrypt0", h'', aad_array_bstr]
  CborEncoder outer, outer_arr;
  cbor_encoder_init(&outer, buf, buf_len, 0);
  if (cbor_encoder_create_array(&outer, &outer_arr, 3) != CBOR_NO_ERROR)
    return 0;
  if (cbor_encode_text_stringz(&outer_arr, "Encrypt0") != CBOR_NO_ERROR)
    return 0;
  if (cbor_encode_byte_string(&outer_arr, EMPTY_BUF, 0) != CBOR_NO_ERROR)
    return 0;
  if (cbor_encode_byte_string(&outer_arr, aad_array_buf, aad_array_len) != CBOR_NO_ERROR)
    return 0;
  if (cbor_encoder_close_container(&outer, &outer_arr) != CBOR_NO_ERROR)
    return 0;
  return cbor_encoder_get_buffer_size(&outer, buf);
}

// Encrypt inner CoAP bytes (code + options + payload) into out_buf.
// Returns 0 on failure. For notifications, advances sender_seq_no_.
size_t CoapServer::oscore_protect_response_(const uint8_t *inner, size_t inner_len, const OscoreRequestInfo &req_info,
                                            bool is_notification, uint8_t *out_buf, size_t out_buf_len) {
  if (inner_len == 0)
    return 0;  // oscore_build_inner_cbor() found the payload too large
  uint8_t piv[5];
  uint8_t piv_len = 0;
  uint8_t kid[8];
  uint8_t kid_len = 0;

  if (is_notification) {
    if (this->oscore_sender_seq_no_ == UINT32_MAX) {
      // Permanent: no notification can be protected again under these keys
      this->status_set_error(LOG_STR("OSCORE sender sequence number exhausted"));
      return 0;
    }
    // A number not covered by a saved threshold would be used again after a reboot
    if (!this->oscore_seq_no_saved_())
      return 0;
    piv_len = oscore_encode_piv(this->oscore_sender_seq_no_, piv);
    // Notification nonce uses server's sender_id as KID
    memcpy(kid, this->oscore_sender_id_buf_, this->oscore_sender_id_len_);
    kid_len = this->oscore_sender_id_len_;
    this->oscore_sender_seq_no_++;
  } else {
    // Simple response: reuse the request's PIV and KID for nonce (RFC 8613 §8.3)
    memcpy(piv, req_info.piv, req_info.piv_len);
    piv_len = req_info.piv_len;
    memcpy(kid, req_info.kid, req_info.kid_len);
    kid_len = req_info.kid_len;
  }

  uint8_t nonce[OSCORE_IV_LEN];
  oscore_build_nonce(piv, piv_len, kid, kid_len, this->oscore_common_iv_, nonce);
  uint8_t aad_buf[80];
  size_t aad_len =
      oscore_build_aad(req_info.kid, req_info.kid_len, req_info.piv, req_info.piv_len, aad_buf, sizeof(aad_buf));
  if (aad_len == 0)
    return 0;

  size_t out_len = 0;
  if (!oscore_aes_ccm_encrypt(this->oscore_sender_key_, nonce, aad_buf, aad_len, inner, inner_len, out_buf, out_buf_len,
                              &out_len))
    return 0;
  ESP_LOGV(TAG, "OSCORE: response encrypted (%s ciphertext=%u bytes)",
           is_notification ? LOG_STR_LITERAL("notification") : LOG_STR_LITERAL("reply"), (unsigned) out_len);
  return out_len;
}

bool CoapServer::oscore_unprotect_core_(const uint8_t *opt_val, uint8_t opt_len, const uint8_t *ciphertext,
                                        uint16_t ciphertext_len, uint8_t *plaintext, size_t plaintext_buf_len,
                                        size_t *plaintext_len, OscoreRequestInfo *req_info) {
  uint8_t flags = (opt_len > 0) ? opt_val[0] : 0;
  uint8_t piv_len = flags & 0x07;
  bool has_kid_ctx = (flags >> 4) & 1;  // RFC 8613 Table 1: bit 4 = h (KID Context flag)
  bool has_kid = (flags >> 3) & 1;      // RFC 8613 Table 1: bit 3 = k (KID flag)

  uint8_t pos = (opt_len > 0) ? 1u : 0u;
  if (piv_len > sizeof(req_info->piv)) {  // 6 and 7 are reserved (RFC 8613 §6.1)
    ESP_LOGW(TAG, "OSCORE: reserved PIV length %u", piv_len);
    return false;
  }
  // Every request carries a Partial IV (RFC 8613 §6.1); without one there is nothing to check replays against
  if (piv_len == 0) {
    ESP_LOGW(TAG, "OSCORE: request without Partial IV");
    return false;
  }
  if (pos + piv_len > opt_len) {
    ESP_LOGW(TAG, "OSCORE: PIV overflow pos=%u piv_len=%u opt_len=%u", pos, piv_len, opt_len);
    return false;
  }
  memcpy(req_info->piv, opt_val + pos, piv_len);
  pos += piv_len;
  req_info->piv_len = piv_len;
  if (has_kid_ctx) {
    if (pos >= opt_len) {
      ESP_LOGW(TAG, "OSCORE: KID context overflow pos=%u opt_len=%u", pos, opt_len);
      return false;
    }
    const uint8_t ctx_len = opt_val[pos++];
    // Checked against what is left, so pos stays within the option and the KID length below cannot wrap
    if (ctx_len > opt_len - pos) {
      ESP_LOGW(TAG, "OSCORE: KID context length %u past the option end", ctx_len);
      return false;
    }
    pos += ctx_len;
  }
  req_info->kid_len = 0;
  if (has_kid) {
    uint8_t kid_len = (uint8_t) (opt_len - pos);
    if (kid_len > sizeof(req_info->kid))
      return false;
    memcpy(req_info->kid, opt_val + pos, kid_len);
    req_info->kid_len = kid_len;
  }

  uint32_t seq = 0;
  for (uint8_t i = 0; i < piv_len; i++)
    seq = (seq << 8) | req_info->piv[i];
  bool replayed = false;
  if (this->oscore_replay_mask_ != 0 && seq <= this->oscore_replay_top_) {
    uint32_t offset = this->oscore_replay_top_ - seq;
    replayed = offset >= 64 || (this->oscore_replay_mask_ & (1ULL << offset));
  }

  uint8_t nonce[OSCORE_IV_LEN];
  oscore_build_nonce(req_info->piv, piv_len, req_info->kid, req_info->kid_len, this->oscore_common_iv_, nonce);
  uint8_t aad_buf[80];
  size_t aad_len = oscore_build_aad(req_info->kid, req_info->kid_len, req_info->piv, piv_len, aad_buf, sizeof(aad_buf));
  if (aad_len == 0) {
    ESP_LOGE(TAG, "OSCORE: AAD build failed");
    return false;
  }

  size_t out_len = 0;
  if (!oscore_aes_ccm_decrypt(this->oscore_recipient_key_, nonce, aad_buf, aad_len, ciphertext, ciphertext_len,
                              plaintext, plaintext_buf_len, &out_len))
    return false;

  // Only a genuine request gets an Echo challenge, so others cannot make the device use up sequence numbers
  if (replayed) {
    ESP_LOGW(TAG, "OSCORE: replayed sequence number %" PRIu32, seq);
    req_info->echo_required = true;
    return false;
  }
  if (this->oscore_replay_mask_ == 0) {
    // After a reboot the window is empty: only a request returning this boot's Echo value can start it
    size_t payload_offset = 0;
    const uint8_t *echo = nullptr;
    size_t echo_len = 0;
    if (!oscore_walk_inner(plaintext, out_len, COAP_OPTION_ECHO, &payload_offset, &echo, &echo_len) ||
        echo == nullptr || echo_len != sizeof(this->oscore_echo_) ||
        memcmp(echo, this->oscore_echo_, sizeof(this->oscore_echo_)) != 0) {
      ESP_LOGD(TAG, "OSCORE: first request since boot, asking for Echo");
      req_info->echo_required = true;
      return false;
    }

    // First request since boot: every older number counts as seen, so requests recorded before the reboot fail
    this->oscore_replay_top_ = seq;
    this->oscore_replay_mask_ = ~0ULL;
  } else if (seq > this->oscore_replay_top_) {
    uint32_t shift = seq - this->oscore_replay_top_;
    this->oscore_replay_mask_ = (shift < 64) ? (this->oscore_replay_mask_ << shift) : 0;
    this->oscore_replay_top_ = seq;
    this->oscore_replay_mask_ |= 1ULL;
  } else {
    uint32_t offset = this->oscore_replay_top_ - seq;
    this->oscore_replay_mask_ |= (1ULL << offset);
  }
  *plaintext_len = out_len;
  ESP_LOGV(TAG, "OSCORE: request decrypted (seq=%" PRIu32 " plaintext=%u bytes)", seq, (unsigned) out_len);
  return true;
}

// static
bool CoapServer::oscore_inner_payload_offset(const uint8_t *plain, size_t len, size_t *offset) {
  const uint8_t *value = nullptr;
  size_t value_len = 0;
  return oscore_walk_inner(plain, len, 0, offset, &value, &value_len);
}

// static
bool CoapServer::oscore_walk_inner(const uint8_t *plain, size_t len, uint16_t number, size_t *offset,
                                   const uint8_t **value, size_t *value_len_out) {
  *value = nullptr;
  *value_len_out = 0;
  uint32_t option = 0;
  size_t pos = 1;  // after the code
  while (pos < len) {
    const uint8_t header = plain[pos++];
    if (header == 0xFF) {
      *offset = pos;
      return true;
    }
    uint32_t delta = header >> 4;
    size_t value_len = header & 0x0F;
    if (delta == 15 || value_len == 15)
      return false;
    if (delta == 13) {
      if (pos + 1 > len)
        return false;
      delta = plain[pos] + 13;
      pos += 1;
    } else if (delta == 14) {
      if (pos + 2 > len)
        return false;
      delta = encode_uint16(plain[pos], plain[pos + 1]) + 269;
      pos += 2;
    }
    option += delta;
    if (value_len == 13) {
      if (pos + 1 > len)
        return false;
      value_len = plain[pos] + 13;
      pos += 1;
    } else if (value_len == 14) {
      if (pos + 2 > len)
        return false;
      value_len = encode_uint16(plain[pos], plain[pos + 1]) + 269;
      pos += 2;
    }
    if (pos > len || value_len > len - pos)
      return false;
    if (number != 0 && option == number) {
      *value = plain + pos;
      *value_len_out = value_len;
    }
    pos += value_len;
  }
  *offset = len;
  return true;
}

size_t CoapServer::oscore_protect_echo_challenge_(const OscoreRequestInfo &req_info, uint8_t *out, size_t out_len,
                                                  uint8_t *opt, uint8_t *opt_len) {
  // 4.01, then Echo (252): delta 13 with extended delta 252 - 13, and its length
  uint8_t inner[3 + OSCORE_ECHO_LEN] = {0x81, static_cast<uint8_t>(0xD0 | OSCORE_ECHO_LEN),
                                        static_cast<uint8_t>(COAP_OPTION_ECHO - 13)};
  memcpy(inner + 3, this->oscore_echo_, OSCORE_ECHO_LEN);
  // Protected like a notification: the device's own Partial IV, never the request's nonce
  size_t cipher_len = this->oscore_protect_response_(inner, sizeof(inner), req_info, true, out, out_len);
  if (cipher_len == 0)
    return 0;
  *opt_len = this->oscore_build_notify_option_(opt);
  return cipher_len;
}

// static
size_t CoapServer::oscore_build_inner_cbor(uint8_t code, const uint8_t *payload, size_t payload_len, uint8_t *out,
                                           size_t out_size) {
  if (out_size < 4 || payload_len > out_size - 4)
    return 0;
  out[0] = code;
  out[1] = 0xC1;  // Content-Format option: delta=12, len=1
  out[2] = 60;    // application/cbor
  out[3] = 0xFF;  // payload marker
  memcpy(out + 4, payload, payload_len);
  return 4 + payload_len;
}

void CoapServer::oscore_restore_seq_no_() {
  this->oscore_seq_pref_ = global_preferences->make_preference<uint32_t>(OSCORE_SEQ_PREF_HASH, true);
  uint32_t stored_threshold = 0;
  this->oscore_seq_pref_.load(&stored_threshold);
  this->oscore_sender_seq_no_ = stored_threshold;
  this->oscore_save_seq_no_();
}

void CoapServer::oscore_save_seq_no_() {
  if (this->oscore_sender_seq_no_ >= OSCORE_SEQ_WARN) {
    ESP_LOGW(TAG, "OSCORE: sender sequence number approaching exhaustion (%" PRIu32 " / %" PRIu32 ")",
             this->oscore_sender_seq_no_, (uint32_t) UINT32_MAX);
  }
  const uint32_t threshold = (this->oscore_sender_seq_no_ > UINT32_MAX - OSCORE_SEQ_INTERVAL)
                                 ? UINT32_MAX
                                 : this->oscore_sender_seq_no_ + OSCORE_SEQ_INTERVAL;
  // Keeping the old threshold retries the save on the next notification
  const bool saved = this->oscore_seq_pref_.save(&threshold) && this->oscore_seq_pref_.sync();
  if (this->status_comm_result(saved, LOG_STR("OSCORE sequence number save failed")))
    this->oscore_seq_threshold_ = threshold;
}

bool CoapServer::oscore_seq_no_saved_() {
  if (this->oscore_sender_seq_no_ < this->oscore_seq_threshold_)
    return true;
  this->oscore_save_seq_no_();
  return this->oscore_sender_seq_no_ < this->oscore_seq_threshold_;
}

#endif  // USE_COAP_OSCORE

#ifdef USE_LOGGER

// static
void CoapServer::log_callback(void *self, uint8_t level, const char *tag, const char *message, size_t message_len) {
  static_cast<CoapServer *>(self)->on_log(level, tag, message, message_len);
}

void CoapServer::log_append_entry_(uint8_t level, const char *tag, const char *message, size_t message_len) {
  // An entry also fits an empty log buffer, after its array start and before the closing break byte
  static constexpr size_t ENTRY_BUF_SIZE = std::min<size_t>(320, LOG_BUF_SIZE - 2);
  uint8_t entry_buf[ENTRY_BUF_SIZE];
  CborEncoder enc, arr;
  cbor_encoder_init(&enc, entry_buf, sizeof(entry_buf), 0);
  if (cbor_encoder_create_array(&enc, &arr, 4) != CBOR_NO_ERROR || cbor_encode_uint(&arr, millis()) != CBOR_NO_ERROR ||
      cbor_encode_uint(&arr, level) != CBOR_NO_ERROR ||
      cbor_encode_text_stringz(&arr, tag != nullptr ? tag : "") != CBOR_NO_ERROR)
    return;
  // A long message is cut to fit, at a UTF-8 character boundary so the text stays valid
  const size_t avail = (size_t) (arr.end - arr.ptr);
  const size_t max_len = avail > 3 ? avail - 3 : 0;  // 3 = text header for up to 65535 bytes
  if (message_len > max_len) {
    message_len = max_len;
    while (message_len > 0 && (static_cast<uint8_t>(message[message_len]) & 0xC0) == 0x80)
      message_len--;
  }
  if (cbor_encode_text_string(&arr, message, message_len) != CBOR_NO_ERROR)
    return;
  cbor_encoder_close_container(&enc, &arr);
  size_t entry_size = cbor_encoder_get_buffer_size(&enc, entry_buf);

  LockGuard guard(this->log_mutex_);
  if (this->log_buf_pos_ + entry_size + 1 > LOG_BUF_SIZE)
    return;
  memcpy(this->log_buf_ + this->log_buf_pos_, entry_buf, entry_size);
  this->log_buf_pos_ += entry_size;
  this->log_buf_has_data_ = true;
}

size_t CoapServer::take_log_payload_(uint8_t *out) {
  LockGuard guard(this->log_mutex_);
  if (!this->log_buf_has_data_)
    return 0;
  this->log_buf_[this->log_buf_pos_] = 0xFF;  // CBOR break byte — closes the indefinite array
  size_t len = this->log_buf_pos_ + 1;
  memcpy(out, this->log_buf_, len);
  this->log_buf_[0] = 0x9F;
  this->log_buf_pos_ = 1;
  this->log_buf_has_data_ = false;
  return len;
}

size_t CoapServer::copy_log_payload_(uint8_t *out) {
  LockGuard guard(this->log_mutex_);
  memcpy(out, this->log_buf_, this->log_buf_pos_);
  out[this->log_buf_pos_] = 0xFF;  // CBOR break byte — closes the indefinite array
  return this->log_buf_pos_ + 1;
}

#endif  // USE_LOGGER

size_t CoapServer::build_ping_payload(uint8_t *buf, bool boot_signal) {
  CborEncoder enc, map;
  cbor_encoder_init(&enc, buf, 16, 0);
  cbor_encoder_create_map(&enc, &map, 1);
  cbor_encode_int(&map, 2);
  if (boot_signal) {
    cbor_encode_int(&map, -1);
  } else {
    cbor_encode_uint(&map, millis() / 1000);
  }
  cbor_encoder_close_container(&enc, &map);
  return cbor_encoder_get_buffer_size(&enc, buf);
}

size_t CoapServer::count_resources() {
  size_t n = 1;  // info only (.well-known/core uses wk_bw_resource_, not resources_)
#ifdef USE_BINARY_SENSOR
  for (auto *e : App.get_binary_sensors()) {
    if (!e->is_internal()) {
      n++;
    }
  }
#endif
#ifdef USE_BUTTON
  for (auto *e : App.get_buttons()) {
    if (!e->is_internal()) {
      n++;
    }
  }
#endif
#ifdef USE_LOCK
  for (auto *e : App.get_locks()) {
    if (!e->is_internal()) {
      n++;
    }
  }
#endif
#ifdef USE_NUMBER
  for (auto *e : App.get_numbers()) {
    if (!e->is_internal()) {
      n++;
    }
  }
#endif
#ifdef USE_SENSOR
  for (auto *e : App.get_sensors()) {
    if (!e->is_internal()) {
      n++;
    }
  }
#endif
#ifdef USE_SWITCH
  for (auto *e : App.get_switches()) {
    if (!e->is_internal()) {
      n += 2;
    }
  }
#endif
#ifdef USE_TEXT_SENSOR
  for (auto *e : App.get_text_sensors()) {
    if (!e->is_internal()) {
      n++;
    }
  }
#endif
#ifdef USE_VALVE
  for (auto *e : App.get_valves()) {
    if (!e->is_internal()) {
      n += 2;
    }
  }
#endif
#ifdef USE_LOGGER
  n++;
#endif
  return n;
}

uint16_t CoapServer::format_link_entry(char *buf, size_t buf_len, const LinkFormatResource &res, bool add_comma) {
  char *p = buf;
  const char *end = buf + buf_len;
  auto put = [&](const char *s, size_t n) {
    size_t avail = (size_t) (end - p);
    if (n > avail)
      n = avail;
    memcpy(p, s, n);
    p += n;
  };
  // Text from the entity goes inside a quoted-string, where '"' and '\' need a backslash (RFC 6690, RFC 2616 §2.2)
  auto put_quoted = [&](const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
      if (s[i] == '"' || s[i] == '\\')
        put("\\", 1);
      put(s + i, 1);
    }
  };
  if (add_comma)
    put(",", 1);
  put("</", 2);
  put(res.path, strlen(res.path));
  put(">", 1);
  if (res.type == ENTITYTYPE_DEVICE) {
    put(R"(;rt="esphome.device";if="if.a";ct=60)", sizeof(R"(;rt="esphome.device";if="if.a";ct=60)") - 1);
    return static_cast<uint16_t>(p - buf);
  }
#ifdef USE_LOGGER
  if (res.type == ENTITYTYPE_LOG) {
    put(R"(;rt="esphome.log";if="if.s";ct=112;obs)", sizeof(R"(;rt="esphome.log";if="if.s";ct=112;obs)") - 1);
    return static_cast<uint16_t>(p - buf);
  }
#endif
  if (res.action != ACTIONTYPE_NO_ACTION) {
    put(R"(;rt="esphome.action";if="if.a";ct=112)", sizeof(R"(;rt="esphome.action";if="if.a";ct=112)") - 1);
    const char *action_title;
    switch (res.action) {
      case ACTIONTYPE_TOGGLE:
        action_title = "toggle";
        break;
      case ACTIONTYPE_STOP:
        action_title = "stop";
        break;
      default:
        action_title = nullptr;
    }
    if (action_title != nullptr) {
      put(";title=\"", sizeof(";title=\"") - 1);
      put(action_title, strlen(action_title));
      put("\"", 1);
    }
#ifdef USE_DEVICES
    if (res.device_index > 0) {
      char dv_buf[8];
      size_t dv_len = (size_t) snprintf(dv_buf, sizeof(dv_buf), ";dv=%u", res.device_index);
      put(dv_buf, dv_len);
    }
#endif
    return static_cast<uint16_t>(p - buf);
  }
  put(";rt=\"esphome.", sizeof(";rt=\"esphome.") - 1);
  put(res.domain, strlen(res.domain));
  if (res.observable) {
    put(R"(";if="if.s";ct=112;obs)", sizeof(R"(";if="if.s";ct=112;obs)") - 1);
  } else {
    put(R"(";if="if.a";ct=112)", sizeof(R"(";if="if.a";ct=112)") - 1);
  }
  if (res.entity != nullptr) {
    put(";title=\"", sizeof(";title=\"") - 1);
    char name_buf[ENTITY_NAME_BUF_SIZE];
    const StringRef name = res.entity->get_name_to(name_buf);
    put_quoted(name.c_str(), name.size());
    put("\";oid=", sizeof("\";oid=") - 1);
    char hash_buf[11];
    size_t hash_len = (size_t) snprintf(hash_buf, sizeof(hash_buf), "%" PRIu32, res.entity->get_object_id_hash());
    put(hash_buf, hash_len);
#ifdef USE_DEVICES
    if (res.device_index > 0) {
      char dv_buf[8];
      size_t dv_len = (size_t) snprintf(dv_buf, sizeof(dv_buf), ";dv=%u", res.device_index);
      put(dv_buf, dv_len);
    }
#endif
#ifdef USE_SENSOR
    if (res.type == ENTITYTYPE_SENSOR) {
      const auto uom_ref = res.entity->get_unit_of_measurement_ref();
      if (!uom_ref.empty()) {
        put(";uom=\"", sizeof(";uom=\"") - 1);
        put_quoted(uom_ref.c_str(), uom_ref.size());
        put("\"", 1);
      }
      char ad_buf[8];
      size_t ad_len = (size_t) snprintf(ad_buf, sizeof(ad_buf), ";ad=%d",
                                        static_cast<sensor::Sensor *>(res.entity)->get_accuracy_decimals());
      put(ad_buf, ad_len);
    }
#endif
    if (res.type == ENTITYTYPE_SENSOR || res.type == ENTITYTYPE_TEXT_SENSOR || res.type == ENTITYTYPE_NUMBER) {
      char dc_buf[MAX_DEVICE_CLASS_LENGTH];
      const char *dc = res.entity->get_device_class_to(dc_buf);
      size_t dc_len = strlen(dc);
      if (dc_len > 0) {
        put(";dc=\"", sizeof(";dc=\"") - 1);
        put_quoted(dc, dc_len);
        put("\"", 1);
      }
    }
#ifdef USE_NUMBER
    if (res.type == ENTITYTYPE_NUMBER) {
      const auto *num = static_cast<const number::Number *>(res.entity);
      char val_buf[24];
      if (!std::isnan(num->traits.get_min_value())) {
        size_t n = (size_t) snprintf(val_buf, sizeof(val_buf), ";min=%g", num->traits.get_min_value());
        put(val_buf, n);
      }
      if (!std::isnan(num->traits.get_max_value())) {
        size_t n = (size_t) snprintf(val_buf, sizeof(val_buf), ";max=%g", num->traits.get_max_value());
        put(val_buf, n);
      }
      if (!std::isnan(num->traits.get_step())) {
        size_t n = (size_t) snprintf(val_buf, sizeof(val_buf), ";step=%g", num->traits.get_step());
        put(val_buf, n);
      }
    }
#endif
  }
  return static_cast<uint16_t>(p - buf);
}

#ifdef USE_COAP_OSCORE
uint8_t CoapServer::oscore_build_notify_option_(uint8_t *opt_buf) {
  uint32_t seq = (this->oscore_sender_seq_no_ == 0) ? 0 : (this->oscore_sender_seq_no_ - 1);
  uint8_t piv_len = oscore_encode_piv(seq, opt_buf + 1);
  opt_buf[0] = (uint8_t) (0x08 | piv_len);
  uint8_t pos = 1 + piv_len;
  memcpy(opt_buf + pos, this->oscore_sender_id_buf_, this->oscore_sender_id_len_);
  pos += this->oscore_sender_id_len_;
  return pos;
}
#endif  // USE_COAP_OSCORE

}  // namespace esphome::coap_server
