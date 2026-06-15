#include "coap_server.h"
#ifdef USE_OPENTHREAD
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/components/openthread/openthread.h"
#include "cbor.h"
#include <openthread/ip6.h>

#ifndef SuccessOrExit
#define SuccessOrExit(aStatus) \
  do { \
    if ((aStatus) != 0) { \
      goto exit; \
    } \
  } while (false)
#endif
#ifdef USE_COAP_OSCORE
#include <psa/crypto.h>
#include "nvs.h"
#endif
#ifdef USE_LOGGER
#include "esphome/components/logger/logger.h"
#endif

namespace esphome::coap_server {

static const char *const TAG = "coap_server";

// Determine the response type from the type of request CON or NON
static otCoapType response_type(otMessage *request) {
  return otCoapMessageGetType(request) == OT_COAP_TYPE_CONFIRMABLE ? OT_COAP_TYPE_ACKNOWLEDGMENT
                                                                   : OT_COAP_TYPE_NON_CONFIRMABLE;
}

void CoapServerOT::setup() {
  CoapServer::setup();
  if (this->is_failed())
    return;

  this->resources_.init(CoapServer::count_resources());

  // Lock openthread and start CoAP
  openthread::InstanceLock lock = openthread::InstanceLock::acquire();
  otInstance *instance = lock.get_instance();
  otError err = otCoapStart(instance, USE_COAP_SERVER_PORT);
  if (err != OT_ERROR_NONE) {
    ESP_LOGE(TAG, "otCoapStart failed: %s", otThreadErrorToString(err));
    mark_failed();
    return;
  }
  ESP_LOGI(TAG, "CoAP server started on port %u", USE_COAP_SERVER_PORT);
  // For use in callbacks (in openthread task)
  this->instance_ = instance;

  // Register .well-known/core as a block-wise resource so OT handles blocks 1+ automatically.
  // mTransmitHook is called by ProcessBlock2Request for block_number > 0, using mLastResponse
  // (cached by ProcessBlockwiseSend after block 0) to reconstruct response options.
  ClearAllBytes(this->wk_bw_resource_);
  this->wk_bw_resource_.mUriPath = ".well-known/core";
  this->wk_bw_resource_.mHandler = &CoapServerOT::handle_well_known_core;
  this->wk_bw_resource_.mTransmitHook = &CoapServerOT::wk_blockwise_transmit_hook;
  this->wk_bw_resource_.mReceiveHook = nullptr;
  this->wk_bw_resource_.mContext = this;
  otCoapAddBlockWiseResource(this->instance_, &this->wk_bw_resource_);
  ESP_LOGD(TAG, "Add CoAP BlockWise Resource: /.well-known/core");

  // Configure the /info resource
  this->resources_.push_back(ehCoapResource());
  ehCoapResource *info_resource = &(this->resources_[this->resources_.size() - 1]);
  info_resource->server = this;
  info_resource->observable = false;
  strncpy(info_resource->path, "info", sizeof(info_resource->path));
  strncpy(info_resource->domain, "device", sizeof(info_resource->domain));
  info_resource->type = EntityType::ENTITYTYPE_DEVICE;
  info_resource->mUriPath = info_resource->path;
  info_resource->mHandler = &CoapServerOT::handle_info_request;
  info_resource->mContext = info_resource;
  info_resource->oscore_exempt = true;
  otCoapAddResource(instance, info_resource);
  ESP_LOGD(TAG, "Add CoAP Server Resource: /%s", info_resource->mUriPath);

  // Configure the /ping resource (not in resources_ so it is excluded from .well-known/core)
  this->ping_resource_.server = this;
  this->ping_resource_.observable = false;
  strncpy(this->ping_resource_.path, "ping", sizeof(this->ping_resource_.path));
  this->ping_resource_.mUriPath = this->ping_resource_.path;
  this->ping_resource_.mHandler = &CoapServerOT::handle_ping_request;
  this->ping_resource_.mContext = &this->ping_resource_;
  otCoapAddResource(instance, &this->ping_resource_);
  ESP_LOGD(TAG, "Add CoAP Server Resource: /%s", this->ping_resource_.mUriPath);

  // Add Component Resources in alphabetical order by rt (domain) so that
  // senml indices are assigned alphabetically.
  uint16_t senml_index = 1;
#ifdef USE_BINARY_SENSOR
  for (binary_sensor::BinarySensor *entity : App.get_binary_sensors()) {
    if (entity->is_internal())
      continue;
    add_coap_resource_(EntityType::ENTITYTYPE_BINARY_SENSOR, entity, true, senml_index);
  }
#endif  // USE_BINARY_SENSOR
#ifdef USE_BUTTON
  for (button::Button *entity : App.get_buttons()) {
    if (entity->is_internal())
      continue;
    add_coap_resource_(EntityType::ENTITYTYPE_BUTTON, entity, false, senml_index);
  }
#endif  // USE_BUTTON
#ifdef USE_LOCK
  for (lock::Lock *entity : App.get_locks()) {
    if (entity->is_internal())
      continue;
    add_coap_resource_(EntityType::ENTITYTYPE_LOCK, entity, true, senml_index);
  }
#endif  // USE_LOCK
#ifdef USE_NUMBER
  for (number::Number *entity : App.get_numbers()) {
    if (entity->is_internal())
      continue;
    add_coap_resource_(EntityType::ENTITYTYPE_NUMBER, entity, true, senml_index);
  }
#endif  // USE_NUMBER
#ifdef USE_SENSOR
  for (sensor::Sensor *entity : App.get_sensors()) {
    if (entity->is_internal())
      continue;
    add_coap_resource_(EntityType::ENTITYTYPE_SENSOR, entity, true, senml_index);
  }
#endif  // USE_SENSOR
#ifdef USE_SWITCH
  for (switch_::Switch *entity : App.get_switches()) {
    if (entity->is_internal())
      continue;
    add_coap_resource_(EntityType::ENTITYTYPE_SWITCH, entity, true, senml_index);
  }
#endif  // USE_SWITCH
#ifdef USE_TEXT_SENSOR
  for (text_sensor::TextSensor *entity : App.get_text_sensors()) {
    if (entity->is_internal())
      continue;
    add_coap_resource_(EntityType::ENTITYTYPE_TEXT_SENSOR, entity, true, senml_index);
  }
#endif  // USE_TEXT_SENSOR
#ifdef USE_VALVE
  for (valve::Valve *entity : App.get_valves()) {
    if (entity->is_internal())
      continue;
    add_coap_resource_(EntityType::ENTITYTYPE_VALVE, entity, true, senml_index);
  }
#endif  // USE_VALVE
#ifdef USE_LOGGER
  add_coap_resource_(EntityType::ENTITYTYPE_LOG, nullptr, true, senml_index);
  this->logs_resource_ = &(this->resources_[this->resources_.size() - 1]);
  this->log_buf_[0] = 0x9F;  // CBOR indefinite-length array start
  this->log_buf_pos_ = 1;
  if (logger::global_logger != nullptr)
    logger::global_logger->add_log_callback(this, CoapServer::log_callback);
  uint32_t flush_ms = std::max(otLinkGetPollPeriod(instance), (uint32_t) 1000);
  this->set_timeout("log_flush", flush_ms, [this]() { this->flush_logs_(); });
#endif  // USE_LOGGER

}  // setup()

bool CoapServerOT::teardown() {
  {
    std::lock_guard<std::mutex> lock(this->lock_);
    for (ehCoapObserver *cur = this->active_observers_; cur != nullptr;) {
      ehCoapObserver *next = cur->next;
      delete cur;
      cur = next;
    }
    this->active_observers_ = nullptr;
    for (ehCoapObserver *cur = this->free_observers_; cur != nullptr;) {
      ehCoapObserver *next = cur->next;
      delete cur;
      cur = next;
    }
    this->free_observers_ = nullptr;
  }
  openthread::InstanceLock lock = openthread::InstanceLock::acquire();
  otInstance *instance = lock.get_instance();
  otCoapRemoveBlockWiseResource(instance, &this->wk_bw_resource_);
  for (auto &res : this->resources_)
    otCoapRemoveResource(instance, &res);
  otCoapRemoveResource(instance, &this->ping_resource_);
  otCoapStop(instance);
  return true;
}  // teardown()

void CoapServerOT::dump_config() {
  ESP_LOGCONFIG(TAG, "CoAP Server:\n  Listen Port: %d\n  Resources: %" PRIu32, USE_COAP_SERVER_PORT,
                (uint32_t) (this->resources_.size() - 1));
  if (!this->subscription_confirm_) {
    ESP_LOGCONFIG(TAG,
                  "  Server Ping: interval=%" PRIu32 "s timeout=%" PRIu32 "s\n"
                  "  Client Ping: interval=%" PRIu32 "s timeout=%" PRIu32 "s",
                  this->server_ping_interval_ms_ / 1000,
                  (uint32_t) std::max((this->server_ping_interval_ms_ / 1000.0f * this->server_ping_timeout_ratio_),
                                      (float) 1.0f),
                  this->client_ping_interval_ms_ / 1000,
                  (uint32_t) std::max((this->client_ping_interval_ms_ / 1000.0f * this->client_ping_timeout_ratio_),
                                      (float) 1.0f));
  } else {
    ESP_LOGCONFIG(TAG, "  Observe Retry: %" PRIu8, this->observe_retry_);
  }
#ifdef USE_COAP_OSCORE
  ESP_LOGCONFIG(TAG, "  OSCORE: enabled");
#endif
}  // dump_config()

// static handler — context is CoapServerOT* (set via wk_bw_resource_.mContext = this)
void CoapServerOT::handle_well_known_core(void *context, otMessage *message, const otMessageInfo *message_info) {
  if (otCoapMessageGetCode(message) != OT_COAP_CODE_GET)
    return;

  CoapServerOT *self = static_cast<CoapServerOT *>(context);
  otInstance *instance = self->instance_;

  // Refresh wk_source_ each call (idempotent; link_format_buf_ is read-only after setup()).
  // OT holds &wk_source_ as ctx for blocks 1+, so it must outlive the handler — member storage satisfies this.
  self->wk_source_.data = self->link_format_buf_.get();
  self->wk_source_.data_len = self->link_format_size_;
  self->wk_source_.content_format = OT_COAP_OPTION_CONTENT_FORMAT_LINK_FORMAT;
  self->wk_source_.read_fn = nullptr;

  otError error = OT_ERROR_NONE;
  otMessage *response = otCoapNewMessage(instance, nullptr);
  if (response == nullptr)
    return;

  // Read Block2 hint from request: Block2(NUM=0, M=false, SZX) means "I can handle SZX-sized blocks".
  otCoapOptionIterator iterator;
  uint64_t block2_value = 0;
  bool has_block2 = (otCoapOptionIteratorInit(&iterator, message) == OT_ERROR_NONE &&
                     otCoapOptionIteratorGetFirstOptionMatching(&iterator, OT_COAP_OPTION_BLOCK2) != nullptr &&
                     otCoapOptionIteratorGetOptionUintValue(&iterator, &block2_value) == OT_ERROR_NONE);

  SuccessOrExit(error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_CONTENT));
  SuccessOrExit(error = otCoapMessageAppendContentFormatOption(
                    response, static_cast<otCoapOptionContentFormat>(self->wk_source_.content_format)));

  if (has_block2) {
    // Echo Block2 back with NUM=0, M=1: ProcessBlockwiseSend reads this to drive the first block,
    // then caches the response (mLastResponse) so ProcessBlock2Request can serve blocks 1+.
    auto szx = static_cast<otCoapBlockSzx>(block2_value & 0x7);
    SuccessOrExit(error = otCoapMessageAppendBlock2Option(response, 0, true, szx));
    SuccessOrExit(error = otCoapMessageSetPayloadMarker(response));
    SuccessOrExit(error = otCoapSendResponseBlockWise(instance, response, message_info, &self->wk_source_,
                                                      &CoapServer::blockwise_transmit_hook));
  } else {
    // No Block2 hint — send inline (payload must fit in one UDP packet).
    SuccessOrExit(error = otCoapMessageSetPayloadMarker(response));
    SuccessOrExit(
        error = otMessageAppend(response, self->wk_source_.data, static_cast<uint16_t>(self->wk_source_.data_len)));
    SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
  }

exit:
  if (error != OT_ERROR_NONE) {
    ESP_LOGE(TAG, "coap send response error %d: %s", error, otThreadErrorToString(error));
    if (response != nullptr)
      otMessageFree(response);
  }
}  // handle_well_known_core

// static — transmit-hook wrapper for blocks 1+: ProcessBlock2Request calls mTransmitHook(mContext, ...)
// where mContext = CoapServerOT*.  Delegates to CoapServer::blockwise_transmit_hook with &wk_source_.
otError CoapServerOT::wk_blockwise_transmit_hook(void *ctx, uint8_t *block, uint32_t pos, uint16_t *len, bool *more) {
  CoapServerOT *self = static_cast<CoapServerOT *>(ctx);
  return CoapServer::blockwise_transmit_hook(&self->wk_source_, block, pos, len, more);
}

// static handler
void CoapServerOT::handle_notification_ack(void *context, otMessage *message, const otMessageInfo *message_info,
                                           otError error) {
  if (error == OT_ERROR_NONE) {
    ESP_LOGV(TAG, "Received coap notification ACK");
  } else {
    ESP_LOGI(TAG, "Notification ACK failed: %s, cancelling observer", otThreadErrorToString(error));
    ehCoapObserver *observer = static_cast<ehCoapObserver *>(context);
    CoapServerOT *server = nullptr;
    {
      std::lock_guard<std::mutex> lock(static_cast<CoapServerOT *>(global_coap_server)->lock_);
      observer->con_pending = false;
      if (observer->resource != nullptr) {
        server = observer->resource->server;
        if (server != nullptr) {
          server->free_observer_(observer);
        }
      }
    }
  }
}

void CoapServerOT::handle_entity_request(ehCoapResource *resource, otMessage *message,
                                         const otMessageInfo *message_info, const EntityType type) {
  otError error = OT_ERROR_NONE;
  otMessage *response = nullptr;
  otInstance *instance = this->instance_;
  uint8_t payload_buffer[COAP_PAYLOAD_MAX_SIZE];
  size_t payload_len = 0;
  uint16_t msg_offset;
  uint16_t msg_len;

  this->touch_client_(*message_info);

#ifdef USE_COAP_OSCORE
  uint8_t oscore_plain[256];
  size_t oscore_plain_len = 0;
  OscoreRequestInfo oscore_req_info{};
  if (!this->oscore_unprotect_request_(message, resource, oscore_plain, sizeof(oscore_plain), &oscore_plain_len,
                                       &oscore_req_info)) {
    response = otCoapNewMessage(instance, nullptr);
    if (response) {
      otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_UNAUTHORIZED);
      otCoapSendResponse(instance, response, message_info);
    }
    return;
  }
  // For protected requests, override msg_offset/msg_len to use plaintext.
  // inner layout: [code(1)] [options] [0xFF] [payload]
  // We only need to extract the payload here; code/options are handled below.
  const bool oscore_protected = (oscore_plain_len > 0);
  if (oscore_protected) {
    // Skip code byte and options to find payload
    uint8_t inner_pos = 1;  // skip code
    while (inner_pos < oscore_plain_len) {
      uint8_t delta_len = oscore_plain[inner_pos++];
      if (delta_len == 0xFF)
        break;
      uint8_t opt_delta = (delta_len >> 4) & 0x0F;
      uint8_t opt_len_field = delta_len & 0x0F;
      if (opt_delta == 13)
        inner_pos++;
      else if (opt_delta == 14)
        inner_pos += 2;
      if (opt_len_field == 13)
        inner_pos++;
      else if (opt_len_field == 14)
        inner_pos += 2;
      else
        inner_pos += opt_len_field;
    }
    // inner_pos now points to start of inner payload (after 0xFF marker)
    msg_offset = inner_pos;
    msg_len = (uint16_t) (oscore_plain_len - inner_pos);
  }
#endif  // USE_COAP_OSCORE

  // When OSCORE is active, the outer code is POST (for plain GET) or FETCH (for GET+Observe).
  // The actual method is in oscore_plain[0]. Use the inner code for dispatch.
#ifdef USE_COAP_OSCORE
  const otCoapCode effective_code =
      oscore_protected ? static_cast<otCoapCode>(oscore_plain[0]) : otCoapMessageGetCode(message);
  ESP_LOGV(TAG, "OSCORE handler: path=%s protected=%d plain_len=%u effective_code=0x%02x", resource->mUriPath,
           (int) oscore_protected, (unsigned) oscore_plain_len, (int) effective_code);
#else
  const otCoapCode effective_code = otCoapMessageGetCode(message);
#endif

  response = otCoapNewMessage(instance, nullptr);
  if (response == nullptr)
    return;

  if (type == ENTITYTYPE_UNKNOWN) {
    SuccessOrExit(
        error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_METHOD_NOT_ALLOWED));
    SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
  } else if (effective_code == OT_COAP_CODE_GET) {
    uint8_t observe = 3;
    bool observe_option_present = false;
    ehCoapObserver *observer = nullptr;
    if (resource->observable) {
      observe = this->observe_(message);
      observe_option_present = (observe < 3);
      observer = this->get_observer_(message, message_info);
      ehCoapObserver *obs = nullptr;
      if (observe_option_present && observer != nullptr && observe == 0) {
        observe_option_present = false;
      }
      if (observe_option_present) {
        if (observe == 0) {
          bool is_con = (otCoapMessageGetType(message) == OT_COAP_TYPE_CONFIRMABLE);
          if (this->get_subscription_confirm() != is_con) {
            ESP_LOGW(TAG, "OSCORE: CON/NON mismatch for %s: server_confirm=%d client_is_con=%d", resource->mUriPath,
                     (int) this->get_subscription_confirm(), (int) is_con);
            SuccessOrExit(
                error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_BAD_REQUEST));
            SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
            goto exit;
          }
          {
            std::lock_guard<std::mutex> lock(this->lock_);
            for (ehCoapObserver *stale = this->active_observers_; stale != nullptr; stale = stale->next) {
              if (stale->resource == resource &&
                  otIp6IsAddressEqual(&stale->message_info.mPeerAddr, &message_info->mPeerAddr)) {
                ESP_LOGD(TAG, "Replace stale observer from restarted client");
                this->free_observer_(stale);
                break;
              }
            }
          }
          ESP_LOGD(TAG, "Create Observer");
          otCoapToken token;
          SuccessOrExit(error = otCoapMessageReadToken(message, &token));
          obs = this->new_observer_(resource, *message_info, token, otCoapMessageGetType(message));
#ifdef USE_COAP_OSCORE
          if (obs != nullptr && oscore_protected) {
            memcpy(obs->oscore_req_piv, oscore_req_info.piv, oscore_req_info.piv_len);
            obs->oscore_req_piv_len = oscore_req_info.piv_len;
            memcpy(obs->oscore_req_kid, oscore_req_info.kid, oscore_req_info.kid_len);
            obs->oscore_req_kid_len = oscore_req_info.kid_len;
          }
#endif
          bool client_known;
          {
            std::lock_guard<std::mutex> guard(this->lock_);
            client_known = (this->find_client_(message_info->mPeerAddr) != nullptr);
          }
          if (!client_known)
            this->new_client_(*message_info);
        } else if (observe == 1 && observer != nullptr) {
          ESP_LOGD(TAG, "Cancel Observer");
          std::lock_guard<std::mutex> lock(this->lock_);
          this->free_observer_(observer);
        }
        if (obs != nullptr)
          observer = obs;
      }
    }
    ehCoapResource *cbor_res = (observer != nullptr) ? observer->resource : resource;
    payload_len = cbor_output_(payload_buffer, COAP_PAYLOAD_MAX_SIZE, cbor_res->entity, cbor_res->type);
    if (payload_len == 0) {
      ESP_LOGW(TAG, "cbor_output_ returned 0 for path=%s type=%d", resource->mUriPath, (int) resource->type);
      SuccessOrExit(
          error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_INTERNAL_ERROR));
      SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
      goto exit;
    }
#ifdef USE_COAP_OSCORE
    if (oscore_protected) {
      uint8_t inner[COAP_PAYLOAD_MAX_SIZE + 8];
      size_t inner_len = oscore_build_inner_cbor(0x45, payload_buffer, payload_len, inner);
      uint8_t ciphertext[COAP_PAYLOAD_MAX_SIZE + OSCORE_TAG_LEN + 8];
      size_t cipher_len =
          this->oscore_protect_response_(inner, inner_len, oscore_req_info, false, ciphertext, sizeof(ciphertext));
      if (cipher_len == 0) {
        SuccessOrExit(
            error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_INTERNAL_ERROR));
        SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
        goto exit;
      }
      // Outer: 2.04 Changed + OSCORE option (empty for simple response) + ciphertext
      SuccessOrExit(error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_CHANGED));
      if (resource->observable && observer != nullptr && observe_option_present && observe == 0)
        SuccessOrExit(error = otCoapMessageAppendObserveOption(response, observer->observe_serial++));
      SuccessOrExit(error = otCoapMessageAppendOption(response, 9, 0, nullptr));  // OSCORE option, empty
      SuccessOrExit(error = otCoapMessageSetPayloadMarker(response));
      SuccessOrExit(error = otMessageAppend(response, ciphertext, (uint16_t) cipher_len));
      SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
      goto exit;
    }
#endif  // USE_COAP_OSCORE
    SuccessOrExit(error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_CONTENT));
    if (resource->observable && observer != nullptr && observe_option_present && observe == 0) {
      SuccessOrExit(error = otCoapMessageAppendObserveOption(response, observer->observe_serial++));
    }
    SuccessOrExit(error = otCoapMessageAppendContentFormatOption(response, OT_COAP_OPTION_CONTENT_FORMAT_CBOR));
    SuccessOrExit(error = otCoapMessageSetPayloadMarker(response));
    SuccessOrExit(error = otMessageAppend(response, payload_buffer, (uint16_t) payload_len));
    SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
  } else if ((type == ENTITYTYPE_SWITCH || type == ENTITYTYPE_BUTTON || type == ENTITYTYPE_NUMBER ||
              type == ENTITYTYPE_LOCK || type == ENTITYTYPE_VALVE) &&
             effective_code == OT_COAP_CODE_POST) {
#ifdef USE_COAP_OSCORE
    if (!oscore_protected) {
      msg_offset = otMessageGetOffset(message);
      msg_len = otMessageGetLength(message) - msg_offset;
    }
    // else msg_offset/msg_len already set from plaintext above
#else
    msg_offset = otMessageGetOffset(message);
    msg_len = otMessageGetLength(message) - msg_offset;
#endif
    if (msg_len > 0 && msg_len <= COAP_PAYLOAD_MAX_SIZE) {
      uint8_t msg_buf[COAP_PAYLOAD_MAX_SIZE];
#ifdef USE_COAP_OSCORE
      if (oscore_protected)
        memcpy(msg_buf, oscore_plain + msg_offset, msg_len);
      else
        otMessageRead(message, msg_offset, msg_buf, msg_len);
#else
      otMessageRead(message, msg_offset, msg_buf, msg_len);
#endif
      apply_entity_post(resource->entity, type, resource->action, msg_buf, msg_len);
    }
    payload_len = cbor_output_(payload_buffer, COAP_PAYLOAD_MAX_SIZE, resource->entity, resource->type);

    if (payload_len == 0) {
      SuccessOrExit(
          error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_INTERNAL_ERROR));
      SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
      goto exit;
    }
#ifdef USE_COAP_OSCORE
    if (oscore_protected) {
      uint8_t inner[COAP_PAYLOAD_MAX_SIZE + 8];
      size_t inner_len = oscore_build_inner_cbor(0x44, payload_buffer, payload_len, inner);
      uint8_t ciphertext[COAP_PAYLOAD_MAX_SIZE + OSCORE_TAG_LEN + 8];
      size_t cipher_len =
          this->oscore_protect_response_(inner, inner_len, oscore_req_info, false, ciphertext, sizeof(ciphertext));
      if (cipher_len == 0) {
        SuccessOrExit(
            error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_INTERNAL_ERROR));
        SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
        goto exit;
      }
      SuccessOrExit(error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_CHANGED));
      SuccessOrExit(error = otCoapMessageAppendOption(response, 9, 0, nullptr));  // OSCORE option, empty
      SuccessOrExit(error = otCoapMessageSetPayloadMarker(response));
      SuccessOrExit(error = otMessageAppend(response, ciphertext, (uint16_t) cipher_len));
      SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
      goto exit;
    }
#endif  // USE_COAP_OSCORE
    SuccessOrExit(error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_CHANGED));
    SuccessOrExit(error = otCoapMessageAppendContentFormatOption(response, OT_COAP_OPTION_CONTENT_FORMAT_CBOR));
    SuccessOrExit(error = otCoapMessageSetPayloadMarker(response));
    SuccessOrExit(error = otMessageAppend(response, payload_buffer, (uint16_t) payload_len));
    SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
  } else {
    ESP_LOGW(TAG, "Method Not Allowed for path=%s effective_code=0x%02x", resource->mUriPath, (int) effective_code);
    SuccessOrExit(
        error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_METHOD_NOT_ALLOWED));
    SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
  }
exit:
  if (error != OT_ERROR_NONE) {
    ESP_LOGE(TAG, "coap send response error %d: %s", error, otThreadErrorToString(error));
    if (response != nullptr)
      otMessageFree(response);
  }
}  // handle_entity_request()

// static handler for all entity types except button
void CoapServerOT::handle_entity_request(void *context, otMessage *message, const otMessageInfo *message_info) {
  ehCoapResource *resource = static_cast<ehCoapResource *>(context);
  resource->server->handle_entity_request(resource, message, message_info, resource->type);
}

#ifdef USE_BUTTON
// static handler
void CoapServerOT::handle_button_request(void *context, otMessage *message, const otMessageInfo *message_info) {
  ehCoapResource *resource = static_cast<ehCoapResource *>(context);
  ESP_LOGV(TAG, "handle_button_request");
  resource->server->handle_button_request(resource, message, message_info);
}

void CoapServerOT::handle_button_request(ehCoapResource *resource, otMessage *message,
                                         const otMessageInfo *message_info) {
  otError error = OT_ERROR_NONE;
  otMessage *response = nullptr;
  otInstance *instance = this->instance_;

  this->touch_client_(*message_info);

  response = otCoapNewMessage(instance, nullptr);
  if (response == nullptr) {
    return;
  }

  if (otCoapMessageGetCode(message) == OT_COAP_CODE_POST) {
#ifdef USE_COAP_OSCORE
    uint8_t oscore_plain[64];
    size_t oscore_plain_len = 0;
    OscoreRequestInfo oscore_req_info{};
    if (!this->oscore_unprotect_request_(message, resource, oscore_plain, sizeof(oscore_plain), &oscore_plain_len,
                                         &oscore_req_info)) {
      otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_UNAUTHORIZED);
      otCoapSendResponse(instance, response, message_info);
      return;
    }
#endif
    static_cast<button::Button *>(resource->entity)->press();
#ifdef USE_COAP_OSCORE
    if (oscore_plain_len > 0) {
      // Inner: [2.04 Changed] (no payload for button)
      uint8_t inner[1] = {0x44};
      uint8_t ciphertext[OSCORE_TAG_LEN + 4];
      size_t cipher_len =
          this->oscore_protect_response_(inner, 1, oscore_req_info, false, ciphertext, sizeof(ciphertext));
      if (cipher_len > 0) {
        otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_CHANGED);
        otCoapMessageAppendOption(response, 9, 0, nullptr);
        otCoapMessageSetPayloadMarker(response);
        otMessageAppend(response, ciphertext, (uint16_t) cipher_len);
        otCoapSendResponse(instance, response, message_info);
      }
      goto exit;
    }
#endif
    SuccessOrExit(error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_CHANGED));
    SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
  } else {
    SuccessOrExit(
        error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_METHOD_NOT_ALLOWED));
    SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
  }
exit:
  if (error != OT_ERROR_NONE) {
    ESP_LOGE(TAG, "coap send response error %d: %s", error, otThreadErrorToString(error));
    if (response != nullptr) {
      otMessageFree(response);
    }
  }
}  // handle_button_request()
#endif  // USE_BUTTON

// static handler
void CoapServerOT::handle_info_request(void *context, otMessage *message, const otMessageInfo *message_info) {
  otError error = OT_ERROR_NONE;
  otMessage *response = nullptr;
  ehCoapResource *resource = static_cast<ehCoapResource *>(context);
  CoapServerOT *self = resource->server;
  otInstance *instance = self->instance_;

  if (otCoapMessageGetCode(message) != OT_COAP_CODE_GET)
    return;

  uint8_t payload_buf[512];
  size_t payload_len = encode_device_info(payload_buf, sizeof(payload_buf), self);

  response = otCoapNewMessage(instance, nullptr);
  if (response == nullptr)
    return;

  if (payload_len == 0) {
    SuccessOrExit(
        error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_INTERNAL_ERROR));
    SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
    goto exit;
  }
  SuccessOrExit(error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_CONTENT));
  SuccessOrExit(error = otCoapMessageAppendContentFormatOption(response, OT_COAP_OPTION_CONTENT_FORMAT_CBOR));
  SuccessOrExit(error = otCoapMessageSetPayloadMarker(response));
  SuccessOrExit(error = otMessageAppend(response, payload_buf, (uint16_t) payload_len));
  SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
exit:
  if (error != OT_ERROR_NONE) {
    ESP_LOGE(TAG, "coap send response error %d: %s", error, otThreadErrorToString(error));
    if (response != nullptr)
      otMessageFree(response);
  }
}  // handle_info_request

// static handler
void CoapServerOT::handle_ping_request(void *context, otMessage *message, const otMessageInfo *message_info) {
  ehCoapResource *resource = static_cast<ehCoapResource *>(context);
  CoapServerOT *self = resource->server;
  otInstance *instance = self->instance_;

  if (otCoapMessageGetCode(message) != OT_COAP_CODE_GET)
    return;

  // An incoming ping is evidence the client is still alive; reset its liveness timestamp.
  // Send -1 on the first ping from a client after device boot so HA detects the reboot.
  bool boot_signal = false;
  {
    std::lock_guard<std::mutex> guard(self->lock_);
    ehCoapClient *client = self->find_client_(message_info->mPeerAddr);
    if (client != nullptr) {
      client->last_response_ms = millis();
      client->ping_miss_count = 0;
      if (!client->boot_notified) {
        client->boot_notified = true;
        boot_signal = true;
      }
    } else {
      boot_signal = true;  // peer unknown = first contact after reboot
    }
  }
  otError error = OT_ERROR_NONE;
  otMessage *response = otCoapNewMessage(instance, nullptr);
  if (response == nullptr)
    return;

  uint8_t payload_buf[16];
  size_t payload_len = self->build_ping_payload(payload_buf, boot_signal);

  SuccessOrExit(error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_CONTENT));
  SuccessOrExit(error = otCoapMessageAppendContentFormatOption(response, OT_COAP_OPTION_CONTENT_FORMAT_CBOR));
  SuccessOrExit(error = otCoapMessageSetPayloadMarker(response));
  SuccessOrExit(error = otMessageAppend(response, payload_buf, (uint16_t) payload_len));
  SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
exit:
  if (error != OT_ERROR_NONE) {
    ESP_LOGE(TAG, "coap send response error %d: %s", error, otThreadErrorToString(error));
    if (response != nullptr)
      otMessageFree(response);
  }
}  // handle_ping_request

void CoapServerOT::shrink_observers() {
  std::lock_guard<std::mutex> lock(this->lock_);
  ehCoapObserver *cur = this->free_observers_;
  while (cur != nullptr) {
    ehCoapObserver *next = cur->next;
    delete cur;
    cur = next;
  }
  this->free_observers_ = nullptr;
  this->high_water_mark_ = this->active_count_;
  ESP_LOGD(TAG, "Observer pool shrunk, high water mark reset to %" PRIu8, this->active_count_);
}

void CoapServerOT::republish_all() {
  for (auto &res : this->resources_) {
    if (res.observable && res.entity != nullptr)
      this->on_entity_update(res.entity);
  }
}

// Protected Methods
void CoapServerOT::handle_observer_(ehCoapObserver *observer, ehCoapResource *expected_resource, const uint8_t *payload,
                                    size_t payload_len) {
  otError error = OT_ERROR_NONE;
  otMessage *message = nullptr;

  // Snapshot observer state and validate before acquiring the expensive InstanceLock.
  bool is_con;
  uint32_t serial;
  otCoapToken token;
  otMessageInfo msg_info_copy;
  {
    std::lock_guard<std::mutex> guard(this->lock_);
    if (observer->resource != expected_resource)
      return;
    bool obs_confirmable = (observer->obs_type == OT_COAP_TYPE_CONFIRMABLE);
    is_con = obs_confirmable && (observer->notify_count == 0 || observer->notify_count == 5);
    if (is_con && observer->con_pending)
      return;
    if (obs_confirmable)
      observer->notify_count = (observer->notify_count == 5) ? 1 : (observer->notify_count + 1);
    if (is_con)
      observer->con_pending = true;
    serial = observer->observe_serial++;
    token = observer->coap_token;
    msg_info_copy = observer->message_info;
  }
  openthread::InstanceLock lock = openthread::InstanceLock::acquire();
  otInstance *instance = lock.get_instance();

  message = otCoapNewMessage(instance, nullptr);
  if (message == nullptr) {
    return;
  }
  SuccessOrExit(error = otCoapMessageInit(message, is_con ? OT_COAP_TYPE_CONFIRMABLE : OT_COAP_TYPE_NON_CONFIRMABLE,
                                          OT_COAP_CODE_CONTENT));
  SuccessOrExit(error = otCoapMessageWriteToken(message, &token));
  SuccessOrExit(error = otCoapMessageAppendObserveOption(message, serial));
#ifdef USE_COAP_OSCORE
  {
    uint8_t inner[COAP_PAYLOAD_MAX_SIZE + 8];
    size_t inner_len = oscore_build_inner_cbor(0x45, payload, payload_len, inner);
    // AAD uses the original registration request's KID/PIV per RFC 8613 §8.3
    OscoreRequestInfo aad_info{};
    memcpy(aad_info.piv, observer->oscore_req_piv, observer->oscore_req_piv_len);
    aad_info.piv_len = observer->oscore_req_piv_len;
    memcpy(aad_info.kid, observer->oscore_req_kid, observer->oscore_req_kid_len);
    aad_info.kid_len = observer->oscore_req_kid_len;
    uint8_t ciphertext[COAP_PAYLOAD_MAX_SIZE + OSCORE_TAG_LEN + 8];
    size_t cipher_len =
        this->oscore_protect_response_(inner, inner_len, aad_info, true, ciphertext, sizeof(ciphertext));
    if (cipher_len == 0) {
      error = OT_ERROR_FAILED;
      goto exit;
    }
    uint8_t oscore_opt[16];
    uint8_t opt_pos = this->oscore_build_notify_option_(oscore_opt);
    SuccessOrExit(error = otCoapMessageAppendOption(message, 9, opt_pos, oscore_opt));
    SuccessOrExit(error = otCoapMessageSetPayloadMarker(message));
    SuccessOrExit(error = otMessageAppend(message, ciphertext, (uint16_t) cipher_len));
    SuccessOrExit(error = otCoapSendRequest(instance, message, &msg_info_copy,
                                            is_con ? CoapServerOT::handle_notification_ack : nullptr, observer));
  }
#else
  SuccessOrExit(error = otCoapMessageAppendContentFormatOption(message, OT_COAP_OPTION_CONTENT_FORMAT_CBOR));
  SuccessOrExit(error = otCoapMessageSetPayloadMarker(message));
  SuccessOrExit(error = otMessageAppend(message, payload, (uint16_t) payload_len));
  SuccessOrExit(error = otCoapSendRequest(instance, message, &msg_info_copy,
                                          is_con ? CoapServerOT::handle_notification_ack : nullptr, observer));
#endif  // USE_COAP_OSCORE
exit:
  if (error != OT_ERROR_NONE) {
    ESP_LOGE(TAG, "coap send message error %d: %s", error, otThreadErrorToString(error));
    observer->con_pending = false;
    if (message != nullptr) {
      otMessageFree(message);
    }
  }
}  // handle_observer

// used by add_coap_resource_
void CoapServerOT::add_coap_resource_(EntityType type, EntityBase *entity, bool observable, uint16_t &senml_index) {
  this->resources_.push_back(ehCoapResource());
  ehCoapResource *resource = &(this->resources_[this->resources_.size() - 1]);

  strncpy(resource->domain, entity_type_domain_name(type), sizeof(resource->domain) - 1);
  resource->path[0] = 'f';
  resource->path[1] = 'p';
  resource->path[2] = '/';
  {
    uint8_t pos = 3;
    append_uint16_decimal(resource->path, pos, senml_index++);
    if (type == ENTITYTYPE_SENSOR || type == ENTITYTYPE_BINARY_SENSOR || type == ENTITYTYPE_TEXT_SENSOR ||
        type == ENTITYTYPE_SWITCH || type == ENTITYTYPE_BUTTON || type == ENTITYTYPE_NUMBER ||
        type == ENTITYTYPE_LOCK || type == ENTITYTYPE_VALVE || type == ENTITYTYPE_LOG) {
      resource->path[pos++] = '/';
      resource->path[pos++] = 'g';
      resource->path[pos++] = '/';
      resource->path[pos++] = '1';
    }
    resource->path[pos] = '\0';
  }
  resource->server = this;
  resource->observable = observable;
  resource->entity = entity;
  resource->type = type;
  resource->mUriPath = resource->path;

  resource->mHandler = &CoapServerOT::handle_entity_request;
#ifdef USE_BUTTON
  if (type == ENTITYTYPE_BUTTON)
    resource->mHandler = &CoapServerOT::handle_button_request;
#endif
#ifdef USE_LOGGER
  if (type == ENTITYTYPE_LOG)
    resource->mHandler = &CoapServerOT::handle_logs_request;
#endif
  resource->mContext = resource;
  otCoapAddResource(this->instance_, resource);
  ESP_LOGD(TAG, "Add CoAP Server Resource: /%s", resource->mUriPath);

#ifdef USE_SWITCH
  if (type == ENTITYTYPE_SWITCH) {
    this->resources_.push_back(ehCoapResource());
    ehCoapResource *toggle_resource = &(this->resources_[this->resources_.size() - 1]);
    strcpy(toggle_resource->path, resource->path);                   // "fp/N/g/1"
    toggle_resource->path[strlen(toggle_resource->path) - 1] = '2';  // "fp/N/g/2"
    toggle_resource->server = this;
    toggle_resource->observable = false;
    toggle_resource->entity = entity;
    toggle_resource->type = type;
    toggle_resource->action = ACTIONTYPE_TOGGLE;
    toggle_resource->mUriPath = toggle_resource->path;
    toggle_resource->mHandler = &CoapServerOT::handle_entity_request;
    toggle_resource->mContext = toggle_resource;
    strncpy(toggle_resource->domain, resource->domain, sizeof(toggle_resource->domain));
    otCoapAddResource(this->instance_, toggle_resource);
    ESP_LOGD(TAG, "Add CoAP Server Resource: /%s", toggle_resource->mUriPath);
  }
#endif
#ifdef USE_VALVE
  if (type == ENTITYTYPE_VALVE) {
    this->resources_.push_back(ehCoapResource());
    ehCoapResource *stop_resource = &(this->resources_[this->resources_.size() - 1]);
    strcpy(stop_resource->path, resource->path);                 // "fp/N/g/1"
    stop_resource->path[strlen(stop_resource->path) - 1] = '2';  // "fp/N/g/2"
    stop_resource->server = this;
    stop_resource->observable = false;
    stop_resource->entity = entity;
    stop_resource->type = type;
    stop_resource->action = ACTIONTYPE_STOP;
    stop_resource->mUriPath = stop_resource->path;
    stop_resource->mHandler = &CoapServerOT::handle_entity_request;
    stop_resource->mContext = stop_resource;
    strncpy(stop_resource->domain, resource->domain, sizeof(stop_resource->domain));
    otCoapAddResource(this->instance_, stop_resource);
    ESP_LOGD(TAG, "Add CoAP Server Resource: /%s", stop_resource->mUriPath);
  }
#endif
}

uint8_t CoapServerOT::observe_(otMessage *message) {
  otCoapOptionIterator iter;
  otCoapOptionIteratorInit(&iter, message);
  const otCoapOption *opt = otCoapOptionIteratorGetFirstOptionMatching(&iter, OT_COAP_OPTION_OBSERVE);
  if (opt != nullptr) {
    uint64_t observe_value;
    if (otCoapOptionIteratorGetOptionUintValue(&iter, &observe_value) == OT_ERROR_NONE) {
      if (observe_value == 0)
        return 0;
      if (observe_value == 1)
        return 1;
    }
  }
  return 3;  // doesn't exist
}

ehCoapObserver *CoapServerOT::get_observer_(otMessage *message, const otMessageInfo *message_info) {
  otCoapToken coap_token;
  std::lock_guard<std::mutex> lock(this->lock_);
  if (otCoapMessageReadToken(message, &coap_token) != OT_ERROR_NONE) {
    return nullptr;
  }
  for (ehCoapObserver *obs = this->active_observers_; obs != nullptr; obs = obs->next) {
    if (otCoapMessageAreTokensEqual(&obs->coap_token, &coap_token) &&
        otIp6IsAddressEqual(&obs->message_info.mPeerAddr, &message_info->mPeerAddr)) {
      return obs;
    }
  }
  return nullptr;
}

ehCoapObserver *CoapServerOT::new_observer_(ehCoapResource *resource, const otMessageInfo &message_info,
                                            const otCoapToken &token, otCoapType obs_type) {
  std::lock_guard<std::mutex> lock(this->lock_);
  ehCoapObserver *obs;
  if (this->free_observers_ != nullptr) {
    obs = this->free_observers_;
    this->free_observers_ = obs->next;
    *obs = ehCoapObserver{};
  } else {
    obs = new ehCoapObserver();
  }
  obs->resource = resource;
  obs->message_info = message_info;
  obs->coap_token = token;
  obs->obs_type = obs_type;
  obs->notify_count = 0;
  // Prepend to active list only after full initialisation so on_entity_update never
  // sees a node with a valid next but a null resource.
  obs->next = this->active_observers_;
  this->active_observers_ = obs;
  this->active_count_++;
  if (this->active_count_ > this->high_water_mark_) {
    this->high_water_mark_ = this->active_count_;
  }
  if (obs_type == OT_COAP_TYPE_NON_CONFIRMABLE) {
    ehCoapClient *client = this->find_client_(message_info.mPeerAddr);
    if (client != nullptr)
      client->has_non_observer = true;
  }
  return obs;
}

ehCoapClient *CoapServerOT::new_client_(const otMessageInfo &message_info) {
  ehCoapClient *result = nullptr;
  {
    std::lock_guard<std::mutex> lock(this->lock_);
    for (uint8_t i = 0; i < USE_COAP_SERVER_MAX_CLIENTS; i++) {
      ehCoapClient &client = this->active_clients_[i];
      if (!client.active) {
        client.peer_addr = message_info.mPeerAddr;
        client.peer_port = message_info.mPeerPort;
        client.active = true;
        client.last_ping_sent_ms = 0;
        client.last_response_ms = millis();
        client.ping_miss_count = 0;
        client.has_non_observer = false;
        client.boot_notified = false;
        client.slot = i;
        this->active_client_count_++;
        // Observer is created before the client in handle_state_request, so
        // new_observer_ couldn't find this client to set has_non_observer.
        // Scan active_observers_ here to catch any NON observers already registered.
        for (ehCoapObserver *obs = this->active_observers_; obs != nullptr; obs = obs->next) {
          if (obs->obs_type == OT_COAP_TYPE_NON_CONFIRMABLE &&
              otIp6IsAddressEqual(&obs->message_info.mPeerAddr, &message_info.mPeerAddr)) {
            client.has_non_observer = true;
            break;
          }
        }
        result = &client;
        break;
      }
    }
  }
  if (result != nullptr) {
    char addr_str[OT_IP6_ADDRESS_STRING_SIZE];
    otIp6AddressToString(&result->peer_addr, addr_str, sizeof(addr_str));
    this->client_connected_callback_(std::string(addr_str));
    this->ping_client_(result);
  }
  return result;
}

void CoapServerOT::ping_client_(ehCoapClient *client) {
  static_assert(USE_COAP_SERVER_MAX_CLIENTS <= 9, "client_ping key assumes single-digit slot");
  char key[] = "client_ping_0";
  key[sizeof("client_ping_") - 1] = '0' + client->slot;
  this->set_timeout(key, this->client_ping_interval_ms_, [this, client]() {
    if (!client->active)
      return;
    uint32_t now = millis();
    bool timed_out = false;
    bool client_recently_active = false;
    uint8_t ping_miss_count = 0;
    {
      std::lock_guard<std::mutex> guard(this->lock_);
      if (client->last_ping_sent_ms != 0) {
        uint32_t timeout_ms =
            (uint32_t) std::max((this->client_ping_interval_ms_ * this->client_ping_timeout_ratio_), (float) 1000.0f);
        timed_out = (now - client->last_response_ms > timeout_ms);
        if (timed_out)
          ping_miss_count = ++client->ping_miss_count;
      }
      client_recently_active = (now - client->last_response_ms < this->client_ping_interval_ms_);
    }
    if (timed_out) {
      if (ping_miss_count >= this->client_ping_retry_) {
        ESP_LOGI(TAG, "Client ping timeout, freeing client slot %u", client->slot);
        this->free_client_(client);
        return;
      }
    }
    if (!client->has_non_observer || client_recently_active) {
      this->ping_client_(client);
      return;
    }
    openthread::InstanceLock lock = openthread::InstanceLock::acquire();
    otInstance *instance = lock.get_instance();
    otMessage *message = otCoapNewMessage(instance, nullptr);
    if (message != nullptr) {
      otCoapMessageInit(message, OT_COAP_TYPE_NON_CONFIRMABLE, OT_COAP_CODE_GET);
      otCoapMessageAppendUriPathOptions(message, "ping");
      otMessageInfo msg_info;
      ClearAllBytes(msg_info);
      msg_info.mPeerAddr = client->peer_addr;
      msg_info.mPeerPort = client->peer_port;
      {
        std::lock_guard<std::mutex> guard(this->lock_);
        client->last_ping_sent_ms = now;
      }
      otError err = otCoapSendRequest(
          instance, message, &msg_info,
          [](void *ctx, otMessage *msg, const otMessageInfo *info, otError error) {
            if (error == OT_ERROR_NONE) {
              ehCoapClient *c = static_cast<ehCoapClient *>(ctx);
              std::lock_guard<std::mutex> guard(static_cast<CoapServerOT *>(global_coap_server)->lock_);
              if (c->active) {
                c->last_response_ms = millis();
                c->ping_miss_count = 0;
              }
            }
          },
          client);
      if (err != OT_ERROR_NONE)
        otMessageFree(message);
    }
    this->ping_client_(client);
  });
}

void CoapServerOT::cancel_ping_client_(ehCoapClient *client) {
  char key[] = "client_ping_0";
  key[sizeof("client_ping_") - 1] = '0' + client->slot;
  this->cancel_timeout(key);
}

ehCoapClient *CoapServerOT::find_client_(const otIp6Address &addr) {
  for (auto &client : this->active_clients_) {
    if (client.active && otIp6IsAddressEqual(&client.peer_addr, &addr))
      return &client;
  }
  return nullptr;
}

void CoapServerOT::touch_client_(const otMessageInfo &message_info) {
  std::lock_guard<std::mutex> guard(this->lock_);
  ehCoapClient *client = this->find_client_(message_info.mPeerAddr);
  if (client != nullptr) {
    client->last_response_ms = millis();
    client->ping_miss_count = 0;
  }
}

void CoapServerOT::free_client_(ehCoapClient *client) {
  this->cancel_ping_client_(client);
  otIp6Address peer_addr;
  {
    std::lock_guard<std::mutex> lock(this->lock_);
    peer_addr = client->peer_addr;
    client->active = false;
    this->active_client_count_--;
  }
  char addr_str[OT_IP6_ADDRESS_STRING_SIZE];
  otIp6AddressToString(&peer_addr, addr_str, sizeof(addr_str));
  this->client_disconnected_callback_(std::string(addr_str));

  // Free all NON observers for this client in one pass under one lock acquisition.
  // CON observers are left alive — they self-cancel via handle_notification_ack on the
  // next confirmable timeout, and freeing them here would race with an in-flight message.
  // The has_non_observer update inside free_observer_ is skipped because find_client_
  // never returns an inactive slot, so we inline the unlink directly.
  {
    std::lock_guard<std::mutex> lock(this->lock_);
    ehCoapObserver **pp = &this->active_observers_;
    while (*pp != nullptr) {
      ehCoapObserver *obs = *pp;
      if (obs->obs_type == OT_COAP_TYPE_NON_CONFIRMABLE &&
          otIp6IsAddressEqual(&obs->message_info.mPeerAddr, &peer_addr)) {
        *pp = obs->next;
        this->active_count_--;
        obs->resource = nullptr;
        obs->next = this->free_observers_;
        this->free_observers_ = obs;
      } else {
        pp = &obs->next;
      }
    }
  }
}

void CoapServerOT::free_observer_(ehCoapObserver *observer) {
  ehCoapObserver **pp = &this->active_observers_;
  while (*pp != nullptr && *pp != observer)
    pp = &(*pp)->next;
  if (*pp == observer) {
    *pp = observer->next;
    this->active_count_--;
  }
  if (observer->obs_type == OT_COAP_TYPE_NON_CONFIRMABLE) {
    bool has_non = false;
    for (ehCoapObserver *o = this->active_observers_; o != nullptr; o = o->next) {
      if (o->obs_type == OT_COAP_TYPE_NON_CONFIRMABLE &&
          otIp6IsAddressEqual(&o->message_info.mPeerAddr, &observer->message_info.mPeerAddr)) {
        has_non = true;
        break;
      }
    }
    if (!has_non) {
      ehCoapClient *client = this->find_client_(observer->message_info.mPeerAddr);
      if (client != nullptr)
        client->has_non_observer = false;
    }
  }
  observer->resource = nullptr;
  observer->next = this->free_observers_;
  this->free_observers_ = observer;
}

void CoapServerOT::notify_observers_(ehCoapResource *resource, const uint8_t *payload, size_t payload_len) {
  // Collect matching observers under lock_ then notify outside it.
  // Holding lock_ while calling handle_observer_ (which acquires InstanceLock) would
  // invert the lock order against OT callbacks that hold InstanceLock and acquire lock_.
  // Storing resource alongside the pointer lets handle_observer_ detect reallocation
  // of a freed node to a different subscriber in the window between collect and send.
  struct Pending {
    ehCoapObserver *observer;
    ehCoapResource *resource;
  };
  // One observer per client per resource at most, so USE_COAP_SERVER_MAX_CLIENTS is a tight bound.
  Pending pending[USE_COAP_SERVER_MAX_CLIENTS];
  size_t pending_count = 0;
  {
    std::lock_guard<std::mutex> lock(this->lock_);
    for (ehCoapObserver *obs = this->active_observers_; obs != nullptr; obs = obs->next) {
      if (obs->resource == resource) {
        ESP_LOGV(TAG, "Found Observer");
        if (pending_count < USE_COAP_SERVER_MAX_CLIENTS) {
          pending[pending_count++] = {obs, obs->resource};
        } else {
          ESP_LOGW(TAG, "Observer notify limit reached, skipping");
        }
      }
    }
  }
  for (size_t i = 0; i < pending_count; i++)
    this->handle_observer_(pending[i].observer, pending[i].resource, payload, payload_len);
}

void CoapServerOT::on_entity_update(EntityBase *entity) {
  if (entity->is_internal())
    return;
  ESP_LOGV(TAG, "On Update");
  ehCoapResource *resource = nullptr;
  for (size_t i = 0; i < this->resources_.size(); i++) {
    if (this->resources_[i].entity == entity && this->resources_[i].observable) {
      resource = &this->resources_[i];
      break;
    }
  }
  if (resource == nullptr)
    return;
  uint8_t payload_buffer[COAP_PAYLOAD_MAX_SIZE];
  size_t payload_len = cbor_output_(payload_buffer, COAP_PAYLOAD_MAX_SIZE, resource->entity, resource->type);
  if (payload_len == 0) {
    ESP_LOGW(TAG, "cbor encode failed, skip notifications");
    return;
  }
  this->notify_observers_(resource, payload_buffer, payload_len);
}

#ifdef USE_COAP_OSCORE

bool CoapServerOT::oscore_unprotect_request_(otMessage *message, const ehCoapResource *resource, uint8_t *plaintext,
                                             size_t plaintext_buf_len, size_t *plaintext_len,
                                             OscoreRequestInfo *req_info) {
  *plaintext_len = 0;
  if (resource->oscore_exempt)
    return true;

  otCoapOptionIterator iter;
  otCoapOptionIteratorInit(&iter, message);
  const otCoapOption *oscore_opt = otCoapOptionIteratorGetFirstOptionMatching(&iter, 9);
  if (oscore_opt == nullptr) {
    ESP_LOGW(TAG, "OSCORE: request on protected resource /%s without OSCORE option", resource->mUriPath);
    return false;
  }
  uint8_t opt_val[32];
  uint16_t opt_len = oscore_opt->mLength;
  if (opt_len > sizeof(opt_val)) {
    ESP_LOGW(TAG, "OSCORE: option value too long (%u bytes)", opt_len);
    return false;
  }
  if (opt_len > 0)
    otCoapOptionIteratorGetOptionValue(&iter, opt_val);

  uint16_t msg_offset = otMessageGetOffset(message);
  uint16_t ciphertext_len = otMessageGetLength(message) - msg_offset;
  if (ciphertext_len == 0 || ciphertext_len > 256) {
    ESP_LOGW(TAG, "OSCORE: unexpected ciphertext length %u", ciphertext_len);
    return false;
  }
  uint8_t ciphertext[256];
  otMessageRead(message, msg_offset, ciphertext, ciphertext_len);

  return this->oscore_unprotect_core_(opt_val, (uint8_t) opt_len, ciphertext, ciphertext_len, plaintext,
                                      plaintext_buf_len, plaintext_len, req_info);
}

#endif  // USE_COAP_OSCORE

#ifdef USE_LOGGER

// static handler
void CoapServerOT::handle_logs_request(void *context, otMessage *message, const otMessageInfo *message_info) {
  ehCoapResource *resource = static_cast<ehCoapResource *>(context);
  CoapServerOT *self = resource->server;
  otInstance *instance = self->instance_;

  otError error = OT_ERROR_NONE;
  otMessage *response = nullptr;

  if (otCoapMessageGetCode(message) != OT_COAP_CODE_GET)
    return;

  self->touch_client_(*message_info);

  response = otCoapNewMessage(instance, nullptr);
  if (response == nullptr)
    return;

  uint8_t observe = self->observe_(message);

  if (observe == 0) {
    bool is_con = (otCoapMessageGetType(message) == OT_COAP_TYPE_CONFIRMABLE);
    if (self->get_subscription_confirm() != is_con) {
      SuccessOrExit(error =
                        otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_BAD_REQUEST));
      SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
      goto exit;
    }
    // Replace any stale observer from the same client IP
    {
      std::lock_guard<std::mutex> lock(self->lock_);
      for (ehCoapObserver *stale = self->active_observers_; stale != nullptr; stale = stale->next) {
        if (stale->resource == resource &&
            otIp6IsAddressEqual(&stale->message_info.mPeerAddr, &message_info->mPeerAddr)) {
          self->free_observer_(stale);
          break;
        }
      }
    }
    otCoapToken token;
    SuccessOrExit(error = otCoapMessageReadToken(message, &token));
    self->new_observer_(resource, *message_info, token, otCoapMessageGetType(message));
  } else if (observe == 1) {
    ehCoapObserver *obs = self->get_observer_(message, message_info);
    if (obs != nullptr) {
      std::lock_guard<std::mutex> lock(self->lock_);
      self->free_observer_(obs);
    }
  }

  static const uint8_t kEmptyArray[] = {0x80};  // CBOR [] — no current state to return
  SuccessOrExit(error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_CONTENT));
  if (observe == 0)
    SuccessOrExit(error = otCoapMessageAppendObserveOption(response, 0));
  SuccessOrExit(error = otCoapMessageAppendContentFormatOption(response, OT_COAP_OPTION_CONTENT_FORMAT_CBOR));
  SuccessOrExit(error = otCoapMessageSetPayloadMarker(response));
  SuccessOrExit(error = otMessageAppend(response, kEmptyArray, sizeof(kEmptyArray)));
  SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));

exit:
  if (error != OT_ERROR_NONE) {
    ESP_LOGE(TAG, "coap logs response error %d: %s", error, otThreadErrorToString(error));
    if (response != nullptr)
      otMessageFree(response);
  }
}  // handle_logs_request

void CoapServerOT::on_log(uint8_t level, const char *tag, const char *message, size_t message_len) {
  {
    std::lock_guard<std::mutex> guard(this->lock_);
    bool found = false;
    for (ehCoapObserver *obs = this->active_observers_; obs != nullptr; obs = obs->next) {
      if (obs->resource == this->logs_resource_) {
        found = true;
        break;
      }
    }
    if (!found)
      return;
  }
  this->log_append_entry_(level, tag, message, message_len);
}

void CoapServerOT::flush_logs_() {
  uint8_t payload[LOG_BUF_SIZE];
  size_t payload_len = this->take_log_payload_(payload);
  if (payload_len > 0)
    this->notify_observers_(this->logs_resource_, payload, payload_len);
  uint32_t flush_ms = std::max(otLinkGetPollPeriod(this->instance_), (uint32_t) 1000);
  this->set_timeout("log_flush", flush_ms, [this]() { this->flush_logs_(); });
}

#endif  // USE_LOGGER

}  // namespace esphome::coap_server
#endif  // USE_OPENTHREAD
