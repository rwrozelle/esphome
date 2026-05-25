#pragma once
#include "esphome/core/defines.h"
#include "esphome/core/component.h"
#include "esphome/core/controller.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/helpers.h"
#include "esphome/core/util.h"

#ifdef USE_OPENTHREAD
#include <openthread/coap.h>
#include <array>
#include <mutex>
#include <vector>
#endif  // USE_OPENTHREAD
#ifdef USE_COAP_OSCORE
#include <psa/crypto.h>
#endif

namespace esphome::coap_server {

class CoapServer;

static constexpr size_t COAP_PAYLOAD_MAX_SIZE = 64;

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

#ifdef USE_OPENTHREAD

template<typename ObjectType> void ClearAllBytes(ObjectType &aObject) {
  memset(reinterpret_cast<void *>(&aObject), 0, sizeof(ObjectType));
}

struct ehCoapResource : otCoapResource {
  CoapServer *server{nullptr};
  bool observable{false};
  char domain[16]{};
  char path[32]{};
  EntityBase *entity{nullptr};
  EntityType type{ENTITYTYPE_UNKNOWN};
  ActionType action{ActionType::ACTIONTYPE_NO_ACTION};
  bool oscore_exempt{false};
  uint8_t device_index{0};
};

struct ehCoapClient {
  otIp6Address peer_addr;
  uint16_t peer_port{0};
  bool active{false};
  uint32_t last_ping_sent_ms{0};
  uint32_t last_response_ms{0};
  uint8_t slot{0};
  uint8_t ping_miss_count{0};
  bool has_non_observer{false};
  bool boot_notified{false};
};

struct ehCoapObserver {
  ehCoapObserver *next{nullptr};
  ehCoapResource *resource{nullptr};
  otMessageInfo message_info;
  otCoapToken coap_token;
  uint32_t observe_serial{0};
  otCoapType obs_type{OT_COAP_TYPE_NON_CONFIRMABLE};
  uint8_t notify_count{0};
  bool con_pending{false};
};

#endif  // USE_OPENTHREAD

class CoapServer final : public Component, public Controller {
 public:
  CoapServer();
  void setup() override;
  bool teardown() override;
  void dump_config() override;
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

#ifdef USE_OPENTHREAD
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
#endif  // USE_OPENTHREAD

#ifdef USE_BINARY_SENSOR
  void on_binary_sensor_update(binary_sensor::BinarySensor *entity) override;
#endif  // USE_BINARY_SENSOR
#ifdef USE_LOCK
  void on_lock_update(lock::Lock *entity) override;
#endif  // USE_LOCK
#ifdef USE_NUMBER
  void on_number_update(number::Number *entity) override;
#endif  // USE_NUMBER
#ifdef USE_SENSOR
  void on_sensor_update(sensor::Sensor *entity) override;
#endif  // USE_SENSOR
#ifdef USE_SWITCH
  void on_switch_update(switch_::Switch *entity) override;
#endif  // USE_SWITCH
#ifdef USE_TEXT_SENSOR
  void on_text_sensor_update(text_sensor::TextSensor *entity) override;
#endif  // USE_TEXT_SENSOR
#ifdef USE_VALVE
  void on_valve_update(valve::Valve *entity) override;
#endif  // USE_VALVE

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

 protected:
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

  static size_t encode_device_info_(uint8_t *buf, size_t buf_len, CoapServer *server);

#ifdef USE_OPENTHREAD
  void add_coap_resource_(EntityType type, EntityBase *entity, bool observable, uint16_t &senml_index);
  size_t cbor_output_(uint8_t *buffer, ehCoapResource *resource);
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
  void on_update_(EntityBase *entity);

#ifdef USE_LOGGER
  void flush_logs_();
  static void log_callback_(void *self, uint8_t level, const char *tag, const char *message, size_t message_len);
  void on_log_(uint8_t level, const char *tag, const char *message, size_t message_len);

  ehCoapResource *logs_resource_{nullptr};
  static constexpr size_t LOG_BUF_SIZE = 1024;
  uint8_t log_buf_[LOG_BUF_SIZE];
  size_t log_buf_pos_{1};  // position 0 is always 0x9F (CBOR indefinite array start)
  bool log_buf_has_data_{false};
  std::mutex log_mutex_;
#endif  // USE_LOGGER

  // Careful: use only inside CoAP callbacks (OpenThread task context)
  // when using for update/write functions, can be used anytime to call read-only functions
  otInstance *instance_;

  ehCoapResource ping_resource_;
  esphome::FixedVector<ehCoapResource> resources_;
  std::array<ehCoapClient, USE_COAP_SERVER_MAX_CLIENTS> active_clients_{};
  ehCoapObserver *active_observers_{nullptr};
  ehCoapObserver *free_observers_{nullptr};
  uint8_t active_count_{0};
  uint8_t high_water_mark_{0};
  mutable std::mutex lock_;

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

  // Derived keys imported as volatile PSA key objects
  psa_key_id_t oscore_sender_key_id_{PSA_KEY_ID_NULL};
  psa_key_id_t oscore_recipient_key_id_{PSA_KEY_ID_NULL};
  uint8_t oscore_common_iv_[OSCORE_IV_LEN]{};

  static constexpr uint32_t OSCORE_SEQ_INTERVAL = 1024;
  uint32_t oscore_sender_seq_no_{0};
  uint32_t oscore_seq_threshold_{0};
  uint32_t oscore_last_seen_seq_{0};

  // Sender ID retained after key derivation for notification OSCORE option
  uint8_t oscore_sender_id_buf_[8]{};
  uint8_t oscore_sender_id_len_{0};

  bool oscore_derive_keys_();
  void oscore_save_seq_no_();
  void oscore_increment_seq_no_();

  // Returns false and sends 4.01 if the request must be OSCORE-protected but isn't.
  // On success fills plaintext/plaintext_len (inner CoAP bytes) and req_info.
  // If resource is oscore_exempt, plaintext is left empty and returns true immediately.
  bool oscore_unprotect_request_(otMessage *message, const ehCoapResource *resource, uint8_t *plaintext,
                                 size_t *plaintext_len, OscoreRequestInfo *req_info);

  // Encrypts inner_payload (code + options + payload) into out_buf.
  // For responses: req_info carries the request's PIV/KID; is_notification uses sender_seq_no_.
  size_t oscore_protect_response_(const uint8_t *inner, size_t inner_len, const OscoreRequestInfo &req_info,
                                  bool is_notification, uint8_t *out_buf, size_t out_buf_len);

  static void oscore_build_nonce_(const uint8_t *piv, uint8_t piv_len, const uint8_t *kid, uint8_t kid_len,
                                  const uint8_t *common_iv, uint8_t nonce[OSCORE_IV_LEN]);
  static size_t oscore_build_aad_(const uint8_t *kid, uint8_t kid_len, const uint8_t *piv, uint8_t piv_len,
                                  uint8_t *buf, size_t buf_len);
#endif  // USE_COAP_OSCORE
#endif  // USE_OPENTHREAD
};

extern CoapServer *global_coap_server;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::coap_server
