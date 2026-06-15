#pragma once
#include "esphome/core/defines.h"
#ifdef USE_WIFI_TWT
#include "esphome/components/wifi_twt/wifi_twt.h"
#endif
#include "esphome/core/component.h"
#include "esphome/core/controller.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/helpers.h"
#include "esphome/core/util.h"
#include <array>
#include <cassert>
#include <memory>

#if defined(USE_LOGGER) || defined(USE_OPENTHREAD)
#include <mutex>
#endif

#ifdef USE_OPENTHREAD
#include <openthread/coap.h>
#else  // !USE_OPENTHREAD
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
// Minimal otError shim so BlockwiseSource / blockwise_transmit_hook compile
// in the Net transport path without pulling in the full OpenThread headers.
typedef int otError;
static constexpr otError OT_ERROR_NONE = 0;
static constexpr otError OT_ERROR_INVALID_ARGS = 7;
#endif  // !USE_OPENTHREAD

#ifdef USE_COAP_OSCORE
#include <psa/crypto.h>
#include <vector>
#endif

namespace esphome::coap_server {

class CoapServer;

static constexpr size_t COAP_PAYLOAD_MAX_SIZE = 256;
static constexpr size_t COAP_PAYLOAD_SMALL_SIZE = 64;
// CBOR buffer sized for the largest entity type actually in the build.
// text_sensor values can fill the full 256 bytes; all other entity types fit in 64.
#ifdef USE_TEXT_SENSOR
static constexpr size_t COAP_CBOR_BUF_SIZE = COAP_PAYLOAD_MAX_SIZE;
#else
static constexpr size_t COAP_CBOR_BUF_SIZE = COAP_PAYLOAD_SMALL_SIZE;
#endif

enum EntityType : uint8_t {
  ENTITYTYPE_UNKNOWN = 0,
  ENTITYTYPE_SENSOR = 1,
  ENTITYTYPE_SWITCH = 2,
  ENTITYTYPE_BINARY_SENSOR = 3,
  ENTITYTYPE_BUTTON = 4,
  ENTITYTYPE_DEVICE = 5,
  ENTITYTYPE_TEXT_SENSOR = 6,
  ENTITYTYPE_NUMBER = 7,
  ENTITYTYPE_LOCK = 8,
  ENTITYTYPE_VALVE = 9,
  ENTITYTYPE_LOG = 10,
};

enum ActionType : uint8_t {
  ACTIONTYPE_NO_ACTION = 0,
  ACTIONTYPE_TOGGLE = 1,
  ACTIONTYPE_TURN_OFF = 2,
  ACTIONTYPE_TURN_ON = 3,
  ACTIONTYPE_LOCK = 4,
  ACTIONTYPE_UNLOCK = 5,
  ACTIONTYPE_OPEN = 6,
  ACTIONTYPE_CLOSE = 7,
  ACTIONTYPE_STOP = 8,
};

// Common resource fields shared by both OT and Net transports.
struct CoapResourceBase {
  char path[32]{};
  char domain[16]{};
  EntityBase *entity{nullptr};
  EntityType type{ENTITYTYPE_UNKNOWN};
  ActionType action{ACTIONTYPE_NO_ACTION};
  bool observable{false};
  bool oscore_exempt{false};
};

// Common observer state shared by both OT and Net transports.
// next and resource are kept in each derived struct because they are self-referential
// typed pointers whose base-typed versions would require pervasive downcasts.
struct CoapObserverBase {
  uint32_t observe_serial{0};
  uint8_t notify_count{0};
  bool con_pending{false};
#ifdef USE_COAP_OSCORE
  uint8_t oscore_req_piv[5]{};
  uint8_t oscore_req_piv_len{0};
  uint8_t oscore_req_kid[8]{};
  uint8_t oscore_req_kid_len{0};
#endif
};

// Common client state shared by both OT and Net transports.
struct CoapClientBase {
  bool active{false};
  uint32_t last_ping_sent_ms{0};
  uint32_t last_response_ms{0};
  uint8_t slot{0};
  uint8_t ping_miss_count{0};
  bool has_non_observer{false};
  bool boot_notified{false};
};

#ifdef USE_OPENTHREAD

class CoapServerOT;

template<typename ObjectType> void ClearAllBytes(ObjectType &aObject) {
  memset(reinterpret_cast<void *>(&aObject), 0, sizeof(ObjectType));
}

// otCoapResource must be first so implicit ehCoapResource* → otCoapResource* needs no pointer adjustment.
struct ehCoapResource : otCoapResource, CoapResourceBase {
  CoapServerOT *server{nullptr};
};

struct ehCoapClient : CoapClientBase {
  otIp6Address peer_addr;
  uint16_t peer_port{0};
};

struct ehCoapObserver : CoapObserverBase {
  ehCoapObserver *next{nullptr};
  ehCoapResource *resource{nullptr};
  otMessageInfo message_info;
  otCoapToken coap_token;
  otCoapType obs_type{OT_COAP_TYPE_NON_CONFIRMABLE};
};

#endif  // USE_OPENTHREAD

// Abstract base — transport-agnostic logic, OSCORE crypto, Controller entity dispatch
class CoapServer : public Component, public Controller {
 public:
  CoapServer();
  void setup() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  template<typename F> void add_on_client_connected_callback(F &&f) {
    this->client_connected_callback_.add(std::forward<F>(f));
  }
  template<typename F> void add_on_client_disconnected_callback(F &&f) {
    this->client_disconnected_callback_.add(std::forward<F>(f));
  }
  uint8_t get_active_client_count() const { return this->active_client_count_; }

  void set_server_ping_interval(uint32_t interval_ms) { this->server_ping_interval_ms_ = interval_ms; }
  uint32_t get_server_ping_interval() const { return this->server_ping_interval_ms_; }
  void set_server_ping_timeout_ratio(float ratio) { this->server_ping_timeout_ratio_ = ratio; }
  float get_server_ping_timeout_ratio() const { return this->server_ping_timeout_ratio_; }
  void set_server_ping_retry(uint8_t retry) { this->server_ping_retry_ = retry; }
  uint8_t get_server_ping_retry() const { return this->server_ping_retry_; }
  void set_client_ping_interval(uint32_t interval_ms) { this->client_ping_interval_ms_ = interval_ms; }
  uint32_t get_client_ping_interval() const { return this->client_ping_interval_ms_; }
  void set_client_ping_timeout_ratio(float ratio) { this->client_ping_timeout_ratio_ = ratio; }
  float get_client_ping_timeout_ratio() const { return this->client_ping_timeout_ratio_; }
  void set_client_ping_retry(uint8_t retry) { this->client_ping_retry_ = retry; }
  uint8_t get_client_ping_retry() const { return this->client_ping_retry_; }
  void set_subscription_confirm(bool confirm) { this->subscription_confirm_ = confirm; }
  bool get_subscription_confirm() const { return this->subscription_confirm_; }
  void set_observe_retry(uint8_t retry) { this->observe_retry_ = retry; }
  uint8_t get_observe_retry() const { return this->observe_retry_; }

#ifdef USE_BINARY_SENSOR
  void on_binary_sensor_update(binary_sensor::BinarySensor *entity) override;
#endif
#ifdef USE_LOCK
  void on_lock_update(lock::Lock *entity) override;
#endif
#ifdef USE_NUMBER
  void on_number_update(number::Number *entity) override;
#endif
#ifdef USE_SENSOR
  void on_sensor_update(sensor::Sensor *entity) override;
#endif
#ifdef USE_SWITCH
  void on_switch_update(switch_::Switch *entity) override;
#endif
#ifdef USE_TEXT_SENSOR
  void on_text_sensor_update(text_sensor::TextSensor *entity) override;
#endif
#ifdef USE_VALVE
  void on_valve_update(valve::Valve *entity) override;
#endif

#ifdef USE_COAP_OSCORE
  void set_oscore_master_secret(std::vector<uint8_t> secret) { this->oscore_master_secret_ = std::move(secret); }
  void set_oscore_master_salt(std::vector<uint8_t> salt) { this->oscore_master_salt_ = std::move(salt); }
  void set_oscore_sender_id(std::vector<uint8_t> id) { this->oscore_sender_id_ = std::move(id); }
  void set_oscore_recipient_id(std::vector<uint8_t> id) { this->oscore_recipient_id_ = std::move(id); }
  void set_oscore_id_context(std::vector<uint8_t> ctx) { this->oscore_id_context_ = std::move(ctx); }

  struct OscoreRequestInfo {
    uint8_t piv[5];
    uint8_t piv_len{0};
    uint8_t kid[8];
    uint8_t kid_len{0};
  };
#endif  // USE_COAP_OSCORE

  // Generic block-wise data source for Block2 GET responses (RFC 7959).
  // Set read_fn to serve streaming data (e.g. OTA firmware); leave nullptr
  // to serve the in-memory data/data_len buffer directly.
  struct BlockwiseSource {
    const uint8_t *data{nullptr};
    size_t data_len{0};
    uint16_t content_format{40};
    otError (*read_fn)(void *ctx, uint8_t *buf, uint32_t pos, uint16_t *len, bool *more){nullptr};
    void *read_ctx{nullptr};
  };

  // OT-compatible transmit hook: fills block_buf from src at position,
  // updates *block_length to bytes written, sets *more. Used directly by
  // otCoapSendResponseBlockWise and by the Net transport's manual Block2 logic.
  static otError blockwise_transmit_hook(void *ctx, uint8_t *block_buf, uint32_t position, uint16_t *block_length,
                                         bool *more);

 protected:
  // Pure virtual — each transport subclass implements entity-state notification
  virtual void on_entity_update(EntityBase *entity) = 0;

  size_t cbor_output_(uint8_t *buffer, size_t buf_len, EntityBase *entity, EntityType type);
  static size_t encode_device_info(uint8_t *buf, size_t buf_len, CoapServer *server);
  // Parses a CBOR POST payload and executes the entity command (switch/number/lock/valve).
  // Used by both OT and Net transports to avoid duplicating the dispatch logic.
  static void apply_entity_post(EntityBase *entity, EntityType type, ActionType action, const uint8_t *payload,
                                size_t payload_len);
  // Encodes {2: millis/1000} (or {2: -1} if boot_signal) as CBOR into buf (caller provides >= 16 bytes).
  static size_t build_ping_payload(uint8_t *buf, bool boot_signal);

  // Counts the total number of resources needed: 2 fixed (well-known/core + info) plus one per
  // non-internal entity (two for Switch and Valve), plus one for the log resource if USE_LOGGER.
  static size_t count_resources();

  // Builds the .well-known/core link-format payload into buf (up to buf_len bytes) and returns
  // the true total size. When buf is nullptr the write is skipped but the true size is still
  // returned, allowing a two-pass allocate + fill pattern.
  static size_t build_link_format(uint8_t *buf, size_t buf_len);

  std::unique_ptr<uint8_t[]> link_format_buf_;
  size_t link_format_size_{0};

  struct LinkFormatResource {
    const char *path;
    const char *domain;
    EntityBase *entity;
    EntityType type;
    ActionType action;
    bool observable;
    uint8_t device_index;
  };
  static uint16_t format_link_entry(char *buf, size_t buf_len, const LinkFormatResource &res, bool add_comma);

  LazyCallbackManager<void(std::string)> client_connected_callback_;
  LazyCallbackManager<void(std::string)> client_disconnected_callback_;
  uint8_t active_client_count_{0};
  uint32_t server_ping_interval_ms_{60000};
  float server_ping_timeout_ratio_{2.5f};
  uint8_t server_ping_retry_{1};
  uint32_t client_ping_interval_ms_{60000};
  float client_ping_timeout_ratio_{2.5f};
  uint8_t client_ping_retry_{1};
  bool subscription_confirm_{false};
  uint8_t observe_retry_{0};

#ifdef USE_LOGGER
  static void log_callback(void *self, uint8_t level, const char *tag, const char *message, size_t message_len);
  virtual void on_log(uint8_t level, const char *tag, const char *message, size_t message_len) {}
  // Encodes [millis, level, tag, message] as a CBOR array and appends it to log_buf_ under log_mutex_.
  void log_append_entry_(uint8_t level, const char *tag, const char *message, size_t message_len);
  // Closes the indefinite CBOR array, copies it out, and resets the buffer.  Returns 0 if no data.
  size_t take_log_payload_(uint8_t *out);

  static constexpr size_t LOG_BUF_SIZE = 1024;
  uint8_t log_buf_[LOG_BUF_SIZE]{};
  size_t log_buf_pos_{1};  // position 0 is always 0x9F (CBOR indefinite array start)
  bool log_buf_has_data_{false};
  std::mutex log_mutex_;
#endif  // USE_LOGGER

#ifdef USE_COAP_OSCORE
  // Key material provided via YAML (cleared after derive)
  std::vector<uint8_t> oscore_master_secret_;
  std::vector<uint8_t> oscore_master_salt_;
  std::vector<uint8_t> oscore_sender_id_;
  std::vector<uint8_t> oscore_recipient_id_;
  std::vector<uint8_t> oscore_id_context_;

  static constexpr size_t OSCORE_KEY_LEN = 16;
  static constexpr size_t OSCORE_IV_LEN = 13;
  static constexpr size_t OSCORE_TAG_LEN = 8;

  // Raw derived key bytes — imported as transient PSA keys per operation
  uint8_t oscore_sender_key_[OSCORE_KEY_LEN]{};
  uint8_t oscore_recipient_key_[OSCORE_KEY_LEN]{};
  uint8_t oscore_common_iv_[OSCORE_IV_LEN]{};

  static constexpr uint32_t OSCORE_SEQ_INTERVAL = 1024;
  static constexpr uint32_t OSCORE_SEQ_WARN = UINT32_MAX - OSCORE_SEQ_INTERVAL;
  uint32_t oscore_sender_seq_no_{0};
  uint32_t oscore_seq_threshold_{0};
  uint32_t oscore_replay_top_{0};
  uint64_t oscore_replay_mask_{0};  // bit i = (top - i) was received; 64-entry window

  // Sender ID retained after key derivation for notification OSCORE option
  uint8_t oscore_sender_id_buf_[8]{};
  uint8_t oscore_sender_id_len_{0};

  bool oscore_derive_keys_();
  void oscore_save_seq_no_();
  void oscore_increment_seq_no_();

  // Encrypts inner_payload (code + options + payload) into out_buf.
  // For responses: req_info carries the request's PIV/KID; is_notification uses sender_seq_no_.
  size_t oscore_protect_response_(const uint8_t *inner, size_t inner_len, const OscoreRequestInfo &req_info,
                                  bool is_notification, uint8_t *out_buf, size_t out_buf_len);

  static void oscore_build_nonce(const uint8_t *piv, uint8_t piv_len, const uint8_t *kid, uint8_t kid_len,
                                 const uint8_t *common_iv, uint8_t nonce[OSCORE_IV_LEN]);
  static size_t oscore_build_aad(const uint8_t *kid, uint8_t kid_len, const uint8_t *piv, uint8_t piv_len, uint8_t *buf,
                                 size_t buf_len);

  // Transport-agnostic OSCORE decrypt core.  Each transport wrapper extracts opt_val and
  // ciphertext from its message type, validates lengths, then delegates here.
  bool oscore_unprotect_core_(const uint8_t *opt_val, uint8_t opt_len, const uint8_t *ciphertext,
                              uint16_t ciphertext_len, uint8_t *plaintext, size_t plaintext_buf_len,
                              size_t *plaintext_len, OscoreRequestInfo *req_info);

  // Builds the OSCORE inner plaintext: [code][0xC1=CF-opt][0x32=cbor][0xFF][payload].
  // Returns total byte count (4 + payload_len). Both transports use this before oscore_protect_response_.
  static size_t oscore_build_inner_cbor(uint8_t code, const uint8_t *payload, size_t payload_len, uint8_t *out);
  // Builds the OSCORE option for outgoing notifications: flags + PIV bytes + sender KID.
  // Writes into opt_buf (caller provides >= 16 bytes). Returns the number of bytes written.
  uint8_t oscore_build_notify_option_(uint8_t *opt_buf);
#endif  // USE_COAP_OSCORE
};

#ifdef USE_OPENTHREAD
// OpenThread transport — uses otInstance, otCoapResource, observer linked list
class CoapServerOT : public CoapServer {
 public:
  void setup() override;
  bool teardown() override;
  void dump_config() override;

  static void handle_well_known_core(void *aContext, otMessage *message, const otMessageInfo *messageInfo);
  static void handle_notification_ack(void *context, otMessage *message, const otMessageInfo *message_info,
                                      otError error);
  static void handle_info_request(void *aContext, otMessage *message, const otMessageInfo *messageInfo);
  static void handle_ping_request(void *aContext, otMessage *message, const otMessageInfo *messageInfo);
  static void handle_entity_request(void *aContext, otMessage *message, const otMessageInfo *messageInfo);
  void handle_entity_request(ehCoapResource *resource, otMessage *message, const otMessageInfo *message_info,
                             const EntityType type);

#ifdef USE_BUTTON
  static void handle_button_request(void *aContext, otMessage *message, const otMessageInfo *messageInfo);
  void handle_button_request(ehCoapResource *resource, otMessage *message, const otMessageInfo *message_info);
#endif
#ifdef USE_LOGGER
  static void handle_logs_request(void *context, otMessage *message, const otMessageInfo *message_info);
#endif  // USE_LOGGER

  void shrink_observers();
  void republish_all();

 protected:
  void on_entity_update(EntityBase *entity) override;

  void add_coap_resource_(EntityType type, EntityBase *entity, bool observable, uint16_t &senml_index);
  void notify_observers_(ehCoapResource *resource, const uint8_t *payload, size_t payload_len);
  void handle_observer_(ehCoapObserver *observer, ehCoapResource *expected_resource, const uint8_t *payload,
                        size_t payload_len);
  ehCoapClient *new_client_(const otMessageInfo &message_info);
  ehCoapClient *find_client_(const otIp6Address &addr);
  void touch_client_(const otMessageInfo &message_info);
  void free_client_(ehCoapClient *client);
  void ping_client_(ehCoapClient *client);
  void cancel_ping_client_(ehCoapClient *client);
  uint8_t observe_(otMessage *message);
  ehCoapObserver *get_observer_(otMessage *message, const otMessageInfo *message_info);
  ehCoapObserver *new_observer_(ehCoapResource *resource, const otMessageInfo &message_info, const otCoapToken &token,
                                otCoapType obs_type);
  void free_observer_(ehCoapObserver *observer);

#ifdef USE_LOGGER
  void flush_logs_();
  void on_log(uint8_t level, const char *tag, const char *message, size_t message_len);
  ehCoapResource *logs_resource_{nullptr};
#endif  // USE_LOGGER

  // Careful: use only inside CoAP callbacks (OpenThread task context)
  otInstance *instance_;

  // Block-wise source for .well-known/core responses; refreshed each handler call.
  // Lives as a member so the ctx pointer given to OT remains valid across block requests.
  BlockwiseSource wk_source_;

  // Transmit-hook wrapper for blocks 1+: receives CoapServerOT* ctx, delegates to
  // CoapServer::blockwise_transmit_hook(&self->wk_source_, ...).
  static otError wk_blockwise_transmit_hook(void *ctx, uint8_t *block, uint32_t pos, uint16_t *len, bool *more);

  // Block-wise resource registration for .well-known/core (otCoapAddBlockWiseResource).
  otCoapBlockwiseResource wk_bw_resource_{};

  ehCoapResource ping_resource_;
  esphome::FixedVector<ehCoapResource> resources_;
  std::array<ehCoapClient, USE_COAP_SERVER_MAX_CLIENTS> active_clients_{};
  ehCoapObserver *active_observers_{nullptr};
  ehCoapObserver *free_observers_{nullptr};
  uint8_t active_count_{0};
  uint8_t high_water_mark_{0};
  mutable std::mutex lock_;

#ifdef USE_COAP_OSCORE
  // Returns false and sends 4.01 if the request must be OSCORE-protected but isn't.
  // On success fills plaintext/plaintext_len (inner CoAP bytes) and req_info.
  // If resource is oscore_exempt, plaintext is left empty and returns true immediately.
  bool oscore_unprotect_request_(otMessage *message, const ehCoapResource *resource, uint8_t *plaintext,
                                 size_t plaintext_buf_len, size_t *plaintext_len, OscoreRequestInfo *req_info);
#endif  // USE_COAP_OSCORE
};
#else  // NOT USE_OPENTHREAD

// ---------------------------------------------------------------------------
// IP/UDP transport — WiFi, Ethernet, Modem
// ---------------------------------------------------------------------------

using NetCoapResource = CoapResourceBase;

struct NetCoapClient : CoapClientBase {
  sockaddr_in6 peer_addr{};
};

struct NetCoapObserver : CoapObserverBase {
  NetCoapObserver *next{nullptr};
  NetCoapResource *resource{nullptr};
  sockaddr_in6 peer_addr{};
  uint8_t token[8]{};
  uint8_t token_len{0};
  bool is_con{false};
  uint16_t con_msg_id{0};  // msg_id of the last CON notification sent
};

// CoAP response builder — fits an IPv6 minimum-MTU datagram (1280 bytes).
// Kept as a class member of CoapServerNet to avoid a 1280-byte stack allocation per handler call.
struct CoapBuilder {
  uint8_t buf[1280];
  size_t pos{0};
  uint16_t last_opt{0};

  void begin(uint8_t type, uint8_t code, uint16_t msg_id, const uint8_t *token, uint8_t token_len) {
    buf[0] = (uint8_t) (0x40 | ((type & 3) << 4) | (token_len & 0x0F));
    buf[1] = code;
    buf[2] = (uint8_t) (msg_id >> 8);
    buf[3] = (uint8_t) (msg_id & 0xFF);
    pos = 4;
    if (token_len > 0 && token != nullptr) {
      memcpy(buf + 4, token, token_len);
      pos += token_len;
    }
    last_opt = 0;
  }

  void option(uint16_t opt_num, const uint8_t *value, uint16_t value_len) {
    uint16_t delta = opt_num - last_opt;
    last_opt = opt_num;
    uint8_t delta4;
    uint8_t ext[2];
    uint8_t ext_len = 0;
    if (delta < 13) {
      delta4 = (uint8_t) delta;
    } else if (delta < 269) {
      delta4 = 13;
      ext[ext_len++] = (uint8_t) (delta - 13);
    } else {
      delta4 = 14;
      uint16_t v = delta - 269;
      ext[ext_len++] = (uint8_t) (v >> 8);
      ext[ext_len++] = (uint8_t) v;
    }
    uint8_t len4;
    uint8_t lext[2];
    uint8_t lext_len = 0;
    if (value_len < 13) {
      len4 = (uint8_t) value_len;
    } else if (value_len < 269) {
      len4 = 13;
      lext[lext_len++] = (uint8_t) (value_len - 13);
    } else {
      len4 = 14;
      uint16_t v = value_len - 269;
      lext[lext_len++] = (uint8_t) (v >> 8);
      lext[lext_len++] = (uint8_t) v;
    }
    assert(pos + 1 + ext_len + lext_len + value_len <= sizeof(buf));
    buf[pos++] = (uint8_t) ((delta4 << 4) | len4);
    for (uint8_t i = 0; i < ext_len; i++)
      buf[pos++] = ext[i];
    for (uint8_t i = 0; i < lext_len; i++)
      buf[pos++] = lext[i];
    if (value_len > 0 && value != nullptr) {
      memcpy(buf + pos, value, value_len);
      pos += value_len;
    }
  }

  void option_uint(uint16_t opt_num, uint32_t value) {
    uint8_t v[4];
    uint8_t vlen = 0;
    if (value > 0xFFFFFF)
      v[vlen++] = (uint8_t) (value >> 24);
    if (value > 0xFFFF)
      v[vlen++] = (uint8_t) (value >> 16);
    if (value > 0xFF)
      v[vlen++] = (uint8_t) (value >> 8);
    if (value > 0)
      v[vlen++] = (uint8_t) value;
    option(opt_num, v, vlen);
  }

  void option_empty(uint16_t opt_num) { option(opt_num, nullptr, 0); }

  void payload_marker() {
    assert(pos + 1 <= sizeof(buf));
    buf[pos++] = 0xFF;
  }

  void append(const uint8_t *data, size_t len) {
    assert(pos + len <= sizeof(buf));
    memcpy(buf + pos, data, len);
    pos += len;
  }
};

class CoapServerNet : public CoapServer {
 public:
  struct CoapPacket {
    uint8_t type{0};  // 0=CON, 1=NON, 2=ACK, 3=RST
    uint8_t code{0};
    uint16_t message_id{0};
    uint8_t token[8]{};
    uint8_t token_len{0};
    bool observe_present{false};
    uint8_t observe{3};  // 0=register, 1=deregister, 3=absent
    uint8_t oscore_opt[32]{};
    uint8_t oscore_opt_len{0};
    const uint8_t *payload{nullptr};
    uint16_t payload_len{0};
    char uri_path[64]{};
    // RFC 7959 Block2 option (23) — for block-wise GET responses
    bool block2_present{false};
    uint32_t block2_num{0};  // block number requested by client
    uint8_t block2_szx{6};   // size exponent: block_size = 16 << szx (default 1024)
  };

  static bool parse_coap(const uint8_t *buf, size_t len, CoapPacket *pkt);

  void setup() override;
  void loop() override;
  bool teardown() override;
  void dump_config() override;
  void republish_all();

 protected:
  void on_entity_update(EntityBase *entity) override;

  void process_datagram_(const uint8_t *buf, size_t len, const sockaddr_in6 *peer);
  virtual void send_response(const uint8_t *buf, size_t len, const sockaddr_in6 *peer);

  void handle_well_known_core_(const CoapPacket &pkt, const sockaddr_in6 &peer);
  void handle_info_request_(const CoapPacket &pkt, const sockaddr_in6 &peer);
  void handle_ping_request_(const CoapPacket &pkt, const sockaddr_in6 &peer);
  void handle_entity_request_(const CoapPacket &pkt, const sockaddr_in6 &peer, NetCoapResource *resource);
#ifdef USE_BUTTON
  void handle_button_request_(const CoapPacket &pkt, const sockaddr_in6 &peer, NetCoapResource *resource);
#endif
#ifdef USE_LOGGER
  void handle_logs_request_(const CoapPacket &pkt, const sockaddr_in6 &peer);
  void flush_logs_();
  void on_log(uint8_t level, const char *tag, const char *message, size_t message_len) override;
  NetCoapResource *logs_resource_{nullptr};
#endif

  void handle_con_response_(const CoapPacket &pkt, const sockaddr_in6 &peer);
  void notify_observers_(NetCoapResource *resource, const uint8_t *payload, size_t payload_len);
  void add_net_resource_(EntityType type, EntityBase *entity, bool observable, uint16_t &senml_index);
  NetCoapResource *find_resource_(const char *path);

  NetCoapClient *new_client_(const sockaddr_in6 &peer);
  NetCoapClient *find_client_(const sockaddr_in6 &peer);
  void touch_client_(const sockaddr_in6 &peer);
  void free_client_(NetCoapClient *client);
  void ping_client_(NetCoapClient *client);
  void cancel_ping_client_(NetCoapClient *client);

  NetCoapObserver *get_observer_(const uint8_t *token, uint8_t token_len, const sockaddr_in6 &peer);
  NetCoapObserver *new_observer_(NetCoapResource *resource, const sockaddr_in6 &peer, const uint8_t *token,
                                 uint8_t token_len, bool is_con);
  void free_observer_(NetCoapObserver *observer);

#ifdef USE_COAP_OSCORE
  bool oscore_unprotect_request_(const CoapPacket &pkt, const NetCoapResource *resource, uint8_t *plaintext,
                                 size_t plaintext_buf_len, size_t *plaintext_len, OscoreRequestInfo *req_info);
#endif

  int sock_{-1};
  uint16_t next_msg_id_{static_cast<uint16_t>(random_uint32())};
  FixedVector<NetCoapResource> resources_;
#ifdef USE_WIFI_TWT
  struct TwtQueueEntry {
    uint8_t data[1280];
    uint16_t len;
    sockaddr_in6 peer;
  };
  StaticRingBuffer<TwtQueueEntry, USE_COAP_SERVER_TWT_QUEUE_DEPTH> twt_queue_{};
  bool twt_queuing_enabled_{false};
  void flush_twt_queue_();
#endif
  std::array<NetCoapClient, USE_COAP_SERVER_MAX_CLIENTS> active_clients_{};
  NetCoapObserver *active_observers_{nullptr};
  NetCoapObserver *free_observers_{nullptr};

  // Moved off the stack to avoid loopTask overflow.
  // recv_buf_: recvfrom staging buffer (1280 B = IPv6 min MTU).
  // builder_: reusable response builder; ESPHome main loop is single-threaded so one instance suffices.
  // cbor_buf_: CBOR output staging; sized to COAP_CBOR_BUF_SIZE (64 B without text_sensor, 256 B with).
  uint8_t recv_buf_[1280]{};
  CoapBuilder builder_{};
  uint8_t cbor_buf_[COAP_CBOR_BUF_SIZE]{};
#ifdef USE_COAP_OSCORE
  // oscore_plain_: decrypted inner CoAP bytes for inbound requests (64 B covers all POST payloads).
  // oscore_inner_/cipher_: staging buffers for outbound OSCORE encryption.
  uint8_t oscore_plain_[COAP_PAYLOAD_SMALL_SIZE]{};
  uint8_t oscore_inner_[COAP_CBOR_BUF_SIZE + 8]{};
  uint8_t oscore_cipher_[COAP_CBOR_BUF_SIZE + 16]{};  // +8 inner overhead + 8 auth tag
#endif
};
#endif  // NOT USE_OPENTHREAD

extern CoapServer *global_coap_server;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// Returns the CoAP domain name string for a given EntityType (e.g. "sensor", "switch").
// Used by both OT and Net transport resource setup.
inline const char *entity_type_domain_name(EntityType type) {
  switch (type) {
#ifdef USE_SENSOR
    case ENTITYTYPE_SENSOR:
      return "sensor";
#endif
#ifdef USE_SWITCH
    case ENTITYTYPE_SWITCH:
      return "switch";
#endif
#ifdef USE_BINARY_SENSOR
    case ENTITYTYPE_BINARY_SENSOR:
      return "binary_sensor";
#endif
#ifdef USE_BUTTON
    case ENTITYTYPE_BUTTON:
      return "button";
#endif
#ifdef USE_TEXT_SENSOR
    case ENTITYTYPE_TEXT_SENSOR:
      return "text_sensor";
#endif
#ifdef USE_NUMBER
    case ENTITYTYPE_NUMBER:
      return "number";
#endif
#ifdef USE_LOCK
    case ENTITYTYPE_LOCK:
      return "lock";
#endif
#ifdef USE_VALVE
    case ENTITYTYPE_VALVE:
      return "valve";
#endif
#ifdef USE_LOGGER
    case ENTITYTYPE_LOG:
      return "log";
#endif
    default:
      return "unknown";
  }
}

// Appends the decimal representation of n to buf[pos], advancing pos.
inline void append_uint16_decimal(char *buf, uint8_t &pos, uint16_t n) {
  if (n == 0) {
    buf[pos++] = '0';
    return;
  }
  uint8_t start = pos;
  while (n > 0) {
    buf[pos++] = '0' + (n % 10);
    n /= 10;
  }
  for (uint8_t i = start, j = (uint8_t) (pos - 1); i < j; i++, j--) {
    char t = buf[i];
    buf[i] = buf[j];
    buf[j] = t;
  }
}

}  // namespace esphome::coap_server
