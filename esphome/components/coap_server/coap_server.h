#pragma once
#include "esphome/core/defines.h"
#include "esphome/core/component.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/entity_includes.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/core/util.h"
#include <array>
#include <memory>

#ifdef USE_COAP_OSCORE
#include <vector>
#include "oscore_backend.h"
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

// Abstract base — transport-agnostic logic, OSCORE crypto, Controller entity dispatch
class CoapServer : public Component {
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
  void on_binary_sensor_update(binary_sensor::BinarySensor *entity);
#endif
#ifdef USE_LOCK
  void on_lock_update(lock::Lock *entity);
#endif
#ifdef USE_NUMBER
  void on_number_update(number::Number *entity);
#endif
#ifdef USE_SENSOR
  void on_sensor_update(sensor::Sensor *entity);
#endif
#ifdef USE_SWITCH
  void on_switch_update(switch_::Switch *entity);
#endif
#ifdef USE_TEXT_SENSOR
  void on_text_sensor_update(text_sensor::TextSensor *entity);
#endif
#ifdef USE_VALVE
  void on_valve_update(valve::Valve *entity);
#endif
  // Not served over CoAP; these stubs only satisfy ControllerContract
#ifdef USE_COVER
  void on_cover_update(cover::Cover *) {}
#endif
#ifdef USE_FAN
  void on_fan_update(fan::Fan *) {}
#endif
#ifdef USE_LIGHT
  void on_light_update(light::LightState *) {}
#endif
#ifdef USE_CLIMATE
  void on_climate_update(climate::Climate *) {}
#endif
#ifdef USE_DATETIME_DATE
  void on_date_update(datetime::DateEntity *) {}
#endif
#ifdef USE_DATETIME_TIME
  void on_time_update(datetime::TimeEntity *) {}
#endif
#ifdef USE_DATETIME_DATETIME
  void on_datetime_update(datetime::DateTimeEntity *) {}
#endif
#ifdef USE_TEXT
  void on_text_update(text::Text *) {}
#endif
#ifdef USE_SELECT
  void on_select_update(select::Select *) {}
#endif
#ifdef USE_MEDIA_PLAYER
  void on_media_player_update(media_player::MediaPlayer *) {}
#endif
#ifdef USE_ALARM_CONTROL_PANEL
  void on_alarm_control_panel_update(alarm_control_panel::AlarmControlPanel *) {}
#endif
#ifdef USE_WATER_HEATER
  void on_water_heater_update(water_heater::WaterHeater *) {}
#endif
#ifdef USE_EVENT
  void on_event(event::Event *) {}
#endif
#ifdef USE_UPDATE
  void on_update(update::UpdateEntity *) {}
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
    bool (*read_fn)(void *ctx, uint8_t *buf, uint32_t pos, uint16_t *len, bool *more){nullptr};
    void *read_ctx{nullptr};
  };

  // Fills block_buf from the BlockwiseSource ctx at position, sets *block_length to the bytes written and
  // *more. Returns false when position is past the end. The OpenThread transport wraps it as its transmit hook.
  static bool blockwise_transmit_hook(void *ctx, uint8_t *block_buf, uint32_t position, uint16_t *block_length,
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
  Mutex log_mutex_;
#endif  // USE_LOGGER

#ifdef USE_COAP_OSCORE
  // Key material provided via YAML (cleared after derive)
  std::vector<uint8_t> oscore_master_secret_;
  std::vector<uint8_t> oscore_master_salt_;
  std::vector<uint8_t> oscore_sender_id_;
  std::vector<uint8_t> oscore_recipient_id_;
  std::vector<uint8_t> oscore_id_context_;

  static constexpr size_t OSCORE_KEY_LEN = OSCORE_AES_KEY_LEN;
  static constexpr size_t OSCORE_IV_LEN = OSCORE_AES_NONCE_LEN;
  static constexpr size_t OSCORE_TAG_LEN = OSCORE_AES_TAG_LEN;

  // Raw derived key bytes — imported as transient PSA keys per operation
  uint8_t oscore_sender_key_[OSCORE_KEY_LEN]{};
  uint8_t oscore_recipient_key_[OSCORE_KEY_LEN]{};
  uint8_t oscore_common_iv_[OSCORE_IV_LEN]{};

  static constexpr uint32_t OSCORE_SEQ_INTERVAL = 1024;
  static constexpr uint32_t OSCORE_SEQ_WARN = UINT32_MAX - OSCORE_SEQ_INTERVAL;
  // fnv1_hash("coap_server_oscore_seq")
  static constexpr uint32_t OSCORE_SEQ_PREF_HASH = 3563581722UL;
  ESPPreferenceObject oscore_seq_pref_;
  uint32_t oscore_sender_seq_no_{0};
  uint32_t oscore_seq_threshold_{0};
  uint32_t oscore_replay_top_{0};
  uint64_t oscore_replay_mask_{0};  // bit i = (top - i) was received; 64-entry window

  // Sender ID retained after key derivation for notification OSCORE option
  uint8_t oscore_sender_id_buf_[8]{};
  uint8_t oscore_sender_id_len_{0};

  bool oscore_derive_keys_();
  // Continues from the stored sequence number and stores the next block
  void oscore_restore_seq_no_();
  void oscore_save_seq_no_();
  void oscore_increment_seq_no_();

  // Encrypts inner_payload (code + options + payload) into out_buf.
  // For responses: req_info carries the request's PIV/KID; is_notification uses sender_seq_no_.
  size_t oscore_protect_response_(const uint8_t *inner, size_t inner_len, const OscoreRequestInfo &req_info,
                                  bool is_notification, uint8_t *out_buf, size_t out_buf_len);

  // Writes seq as the shortest big-endian Partial IV (1 to 4 bytes); returns its length
  static uint8_t oscore_encode_piv(uint32_t seq, uint8_t *piv);
  static void oscore_build_nonce(const uint8_t *piv, uint8_t piv_len, const uint8_t *kid, uint8_t kid_len,
                                 const uint8_t *common_iv, uint8_t nonce[OSCORE_IV_LEN]);
  static size_t oscore_build_aad(const uint8_t *kid, uint8_t kid_len, const uint8_t *piv, uint8_t piv_len, uint8_t *buf,
                                 size_t buf_len);

  // Transport-agnostic OSCORE decrypt core.  Each transport wrapper extracts opt_val and
  // ciphertext from its message type, validates lengths, then delegates here.
  bool oscore_unprotect_core_(const uint8_t *opt_val, uint8_t opt_len, const uint8_t *ciphertext,
                              uint16_t ciphertext_len, uint8_t *plaintext, size_t plaintext_buf_len,
                              size_t *plaintext_len, OscoreRequestInfo *req_info);

  // Builds the OSCORE inner plaintext: [code][0xC1=CF-opt][60=cbor][0xFF][payload].
  // Returns total byte count (4 + payload_len). Both transports use this before oscore_protect_response_.
  static size_t oscore_build_inner_cbor(uint8_t code, const uint8_t *payload, size_t payload_len, uint8_t *out);
  // Builds the OSCORE option for outgoing notifications: flags + PIV bytes + sender KID.
  // Writes into opt_buf (caller provides >= 16 bytes). Returns the number of bytes written.
  uint8_t oscore_build_notify_option_(uint8_t *opt_buf);
#endif  // USE_COAP_OSCORE
};

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
