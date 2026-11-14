#pragma once

#include "coap_server.h"

#ifdef USE_COAP_SERVER_OPENTHREAD

#include "ot_coap_compat.h"

namespace esphome::coap_server {

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
  bool announced{false};  // on_client_connected has run on the main loop
};

struct ehCoapObserver : CoapObserverBase {
  ehCoapObserver *next{nullptr};
  ehCoapResource *resource{nullptr};
  otMessageInfo message_info;
  otCoapToken coap_token;
  otCoapType obs_type{OT_COAP_TYPE_NON_CONFIRMABLE};
  uint32_t notify_round{0};  // the notify_observers_() call that last picked it, see notify_round_
};

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
  // Main loop: runs on_client_connected and starts the pings for a client added on the OpenThread task
  void announce_client_(ehCoapClient *client);
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
  // Called with lock_ held once OpenThread no longer holds observer as a CON context
  void finish_con_(ehCoapObserver *observer);

  // A command (entity POST or button press) taken on the OpenThread task and run on the main loop, which then sends
  // the reply as a separate response under the request's token (RFC 7252 §5.2.2)
  static constexpr size_t COMMAND_PAYLOAD_MAX = 32;
  static constexpr size_t MAX_PENDING_COMMANDS = 4;
  struct PendingCommand {
    ehCoapResource *resource{nullptr};  // nullptr while the slot is free
    otMessageInfo message_info;
    otCoapToken token;
    bool confirmable{false};
    uint8_t payload_len{0};
    uint8_t payload[COMMAND_PAYLOAD_MAX];
#ifdef USE_COAP_OSCORE
    bool oscore_protected{false};
    OscoreRequestInfo oscore_req_info;
#endif
  };
  // OpenThread task: fills cmd from the request, payload included; false when the payload does not fit
  static bool make_command_(ehCoapResource *resource, otMessage *message, const otMessageInfo *message_info,
                            const uint8_t *payload, size_t payload_len, PendingCommand *cmd);
  // OpenThread task: stores cmd, acknowledges a CON request with an empty ACK and hands the command to the main
  // loop, or answers 5.03 when every slot is busy. Sends or frees response.
  void queue_command_(const PendingCommand &cmd, otMessage *message, const otMessageInfo *message_info,
                      otMessage *response);
  // Main loop: runs the command in slot and sends its reply
  void run_command_(uint8_t slot);
  void send_command_reply_(const PendingCommand &cmd, const uint8_t *payload, size_t payload_len);
  std::array<PendingCommand, MAX_PENDING_COMMANDS> pending_commands_{};  // guarded by lock_

#ifdef USE_LOGGER
  void flush_logs_();
  // (Re)starts the log flush interval when the poll period it follows has changed
  void schedule_log_flush_();
  uint32_t log_flush_ms_{0};
  void on_log(uint8_t level, const char *tag, const char *message, size_t message_len);
  ehCoapResource *logs_resource_{nullptr};
#endif  // USE_LOGGER

  // Careful: use only inside CoAP callbacks (OpenThread task context)
  otInstance *instance_;

// Without CONFIG_OPENTHREAD_COAP_BLOCK, the Zephyr OpenThread library has no block-wise transfer (NCS's prebuilt
// library never has it). On ESP32 coap_server turns it on with OPENTHREAD_CONFIG_COAP_BLOCKWISE_TRANSFER_ENABLE.
#if defined(USE_ZEPHYR) && !defined(CONFIG_OPENTHREAD_COAP_BLOCK)
  // Sends the .well-known/core block that block2_value asks for, and frees response on failure
  void send_well_known_block_(otMessage *message, const otMessageInfo *message_info, otMessage *response,
                              uint64_t block2_value);
  otCoapResource wk_resource_{};
#else
  // Block-wise source for .well-known/core responses; refreshed each handler call.
  // Lives as a member so the ctx pointer given to OT remains valid across block requests.
  BlockwiseSource wk_source_;

  // Transmit-hook wrapper for blocks 1+: receives CoapServerOT* ctx, delegates to
  // CoapServer::blockwise_transmit_hook(&self->wk_source_, ...).
  static otError wk_blockwise_transmit_hook(void *ctx, uint8_t *block, uint32_t pos, uint16_t *len, bool *more);
  // Transmit hook for block 0: ctx is a BlockwiseSource
  static otError source_transmit_hook(void *ctx, uint8_t *block, uint32_t pos, uint16_t *len, bool *more);

  // Block-wise resource registration for .well-known/core (otCoapAddBlockWiseResource).
  otCoapBlockwiseResource wk_bw_resource_{};
#endif

  ehCoapResource ping_resource_;
  esphome::FixedVector<ehCoapResource> resources_;
  std::array<ehCoapClient, USE_COAP_SERVER_MAX_CLIENTS> active_clients_{};
  ehCoapObserver *active_observers_{nullptr};
  ehCoapObserver *free_observers_{nullptr};
  // Freed while a CON notification was unanswered; not reused until finish_con_()
  ehCoapObserver *release_observers_{nullptr};
  uint8_t active_count_{0};
  uint8_t high_water_mark_{0};
  uint32_t notify_round_{0};
  mutable Mutex lock_;

#ifdef USE_COAP_OSCORE
  // Returns false when the request must be OSCORE-protected and is not, or fails to decrypt or the replay check.
  // On success fills plaintext/plaintext_len (inner CoAP bytes) and req_info.
  // If resource is oscore_exempt, plaintext is left empty and returns true immediately.
  bool oscore_unprotect_request_(otMessage *message, const ehCoapResource *resource, uint8_t *plaintext,
                                 size_t plaintext_buf_len, size_t *plaintext_len, OscoreRequestInfo *req_info);
  // Answers a refused request: a protected 4.01 with Echo when unprotect asked for one, a plain 4.01 otherwise.
  // Uses response when it is not nullptr, and frees it if sending fails.
  void send_oscore_refusal_(otMessage *response, otMessage *message, const otMessageInfo *message_info,
                            const OscoreRequestInfo &req_info);
#endif  // USE_COAP_OSCORE
};

}  // namespace esphome::coap_server

#endif  // USE_COAP_SERVER_OPENTHREAD
