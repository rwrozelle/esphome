#pragma once

#include "coap_server.h"

#ifdef USE_COAP_SERVER_OPENTHREAD

#include <mutex>
#include <openthread/coap.h>

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
};

struct ehCoapObserver : CoapObserverBase {
  ehCoapObserver *next{nullptr};
  ehCoapResource *resource{nullptr};
  otMessageInfo message_info;
  otCoapToken coap_token;
  otCoapType obs_type{OT_COAP_TYPE_NON_CONFIRMABLE};
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
  // Transmit hook for block 0: ctx is a BlockwiseSource
  static otError source_transmit_hook(void *ctx, uint8_t *block, uint32_t pos, uint16_t *len, bool *more);

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

}  // namespace esphome::coap_server

#endif  // USE_COAP_SERVER_OPENTHREAD
