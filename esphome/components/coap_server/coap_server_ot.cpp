#include "coap_server.h"
#ifdef USE_OPENTHREAD
#include "esphome/core/application.h"
#include "esphome/core/controller_registry.h"
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

// Used when in functions that need to lock CoapServer when they don't directly know CoapServer
CoapServer *global_coap_server = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
CoapServer::CoapServer() { global_coap_server = this; }

// Setup CoapServer
void CoapServer::setup() {
  ControllerRegistry::register_controller(this);
#ifdef USE_COAP_OSCORE
  if (!this->oscore_derive_keys_()) {
    mark_failed();
    return;
  }
  {
    nvs_handle_t nvs_handle;
    uint32_t stored_threshold = 0;
    if (nvs_open("coap_oscore", NVS_READONLY, &nvs_handle) == ESP_OK) {
      nvs_get_u32(nvs_handle, "seq", &stored_threshold);
      nvs_close(nvs_handle);
    }
    this->oscore_sender_seq_no_ = stored_threshold;
    this->oscore_save_seq_no_();
  }
#endif  // USE_COAP_OSCORE

  // Count exactly how many resources will be registered so FixedVector allocates once.
  // .well-known/core + info + 1 per sensor + 2 per switch (state + toggle) + 1 per binary sensor + 1 per button
  // + 1 per text_sensor + 1 per number + 1 per lock + 2 per valve (state + stop)
  size_t resource_count = 2;
#ifdef USE_LOGGER
  resource_count++;
#endif
#ifdef USE_BINARY_SENSOR
  for (binary_sensor::BinarySensor *entity : App.get_binary_sensors()) {
    if (!entity->is_internal())
      resource_count++;
  }
#endif
#ifdef USE_BUTTON
  for (button::Button *entity : App.get_buttons()) {
    if (!entity->is_internal())
      resource_count++;
  }
#endif
#ifdef USE_LOCK
  for (lock::Lock *entity : App.get_locks()) {
    if (!entity->is_internal())
      resource_count++;
  }
#endif
#ifdef USE_NUMBER
  for (number::Number *entity : App.get_numbers()) {
    if (!entity->is_internal())
      resource_count++;
  }
#endif
#ifdef USE_SENSOR
  for (sensor::Sensor *entity : App.get_sensors()) {
    if (!entity->is_internal())
      resource_count++;
  }
#endif
#ifdef USE_SWITCH
  for (switch_::Switch *entity : App.get_switches()) {
    if (!entity->is_internal())
      resource_count += 2;
  }
#endif
#ifdef USE_TEXT_SENSOR
  for (text_sensor::TextSensor *entity : App.get_text_sensors()) {
    if (!entity->is_internal())
      resource_count++;
  }
#endif
#ifdef USE_VALVE
  for (valve::Valve *entity : App.get_valves()) {
    if (!entity->is_internal())
      resource_count += 2;
  }
#endif
  this->resources_.init(resource_count);

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

  // Configure the .well-known/core resource and add
  this->resources_.push_back(ehCoapResource());
  ehCoapResource *resource = &(this->resources_[this->resources_.size() - 1]);
  resource->server = this;
  resource->observable = false;
  resource->mUriPath = ".well-known/core";
  resource->mHandler = &CoapServer::handle_well_known_core;
  resource->mContext = resource;
  resource->oscore_exempt = true;
  otCoapAddResource(this->instance_, resource);
  ESP_LOGD(TAG, "Add CoAP Server Resource: /%s", resource->mUriPath);

  // Configure the /info resource
  this->resources_.push_back(ehCoapResource());
  ehCoapResource *info_resource = &(this->resources_[this->resources_.size() - 1]);
  info_resource->server = this;
  info_resource->observable = false;
  strncpy(info_resource->path, "info", sizeof(info_resource->path));
  strncpy(info_resource->domain, "device", sizeof(info_resource->domain));
  info_resource->type = EntityType::ENTITYTYPE_DEVICE;
  info_resource->mUriPath = info_resource->path;
  info_resource->mHandler = &CoapServer::handle_info_request;
  info_resource->mContext = info_resource;
  info_resource->oscore_exempt = true;
  otCoapAddResource(instance, info_resource);
  ESP_LOGD(TAG, "Add CoAP Server Resource: /%s", info_resource->mUriPath);

  // Configure the /ping resource (not in resources_ so it is excluded from .well-known/core)
  this->ping_resource_.server = this;
  this->ping_resource_.observable = false;
  strncpy(this->ping_resource_.path, "ping", sizeof(this->ping_resource_.path));
  this->ping_resource_.mUriPath = this->ping_resource_.path;
  this->ping_resource_.mHandler = &CoapServer::handle_ping_request;
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
    logger::global_logger->add_log_callback(this, CoapServer::log_callback_);
  uint32_t flush_ms = std::max(otLinkGetPollPeriod(instance), (uint32_t) 1000);
  this->set_timeout("log_flush", flush_ms, [this]() { this->flush_logs_(); });
#endif  // USE_LOGGER

}  // setup()

bool CoapServer::teardown() {
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
  for (auto &res : this->resources_)
    otCoapRemoveResource(instance, &res);
  otCoapRemoveResource(instance, &this->ping_resource_);
  otCoapStop(instance);
  return true;
}  // teardown()

void CoapServer::dump_config() {
  ESP_LOGCONFIG(
      TAG,
      "CoAP Server:\n"
      "  Listen Port: %d\n"
      "  Resources: %" PRIu32 "\n"
      "  Server Ping: interval=%" PRIu32 "s timeout=%" PRIu32 "s\n"
      "  Client Ping: interval=%" PRIu32 "s timeout=%" PRIu32 "s",
      USE_COAP_SERVER_PORT, (uint32_t) (this->resources_.size() - 2), this->server_ping_interval_ms_ / 1000,
      (uint32_t) std::max((this->server_ping_interval_ms_ / 1000.0f * this->server_ping_timeout_ratio_), (float) 1.0f),
      this->client_ping_interval_ms_ / 1000,
      (uint32_t) std::max((this->client_ping_interval_ms_ / 1000.0f * this->client_ping_timeout_ratio_), (float) 1.0f));
#ifdef USE_COAP_OSCORE
  ESP_LOGCONFIG(TAG, "  OSCORE: enabled");
#endif
}  // dump_config()

static uint16_t format_link_entry(char *buf, size_t buf_len, const ehCoapResource &res, bool add_comma) {
  char *p = buf;
  const char *end = buf + buf_len;
  auto put = [&](const char *s, size_t n) {
    size_t avail = (size_t) (end - p);
    if (n > avail)
      n = avail;
    memcpy(p, s, n);
    p += n;
  };
  if (add_comma)
    put(",", 1);
  put("</", 2);
  put(res.path, strlen(res.path));
  put(">", 1);
  if (res.type == ENTITYTYPE_DEVICE) {
    put(";rt=\"esphome.device\";if=\"if.a\";ct=60", sizeof(";rt=\"esphome.device\";if=\"if.a\";ct=60") - 1);
    return static_cast<uint16_t>(p - buf);
  }
#ifdef USE_LOGGER
  if (res.type == ENTITYTYPE_LOG) {
    put(";rt=\"esphome.log\";if=\"if.s\";ct=112;obs", sizeof(";rt=\"esphome.log\";if=\"if.s\";ct=112;obs") - 1);
    return static_cast<uint16_t>(p - buf);
  }
#endif
  if (res.action != ACTIONTYPE_NO_ACTION) {
    put(";rt=\"esphome.action\";if=\"if.a\";ct=112", sizeof(";rt=\"esphome.action\";if=\"if.a\";ct=112") - 1);
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
    put("\";if=\"if.s\";ct=112;obs", sizeof("\";if=\"if.s\";ct=112;obs") - 1);
  } else {
    put("\";if=\"if.a\";ct=112", sizeof("\";if=\"if.a\";ct=112") - 1);
  }
  put(";title=\"", sizeof(";title=\"") - 1);
  const auto &name = res.entity->get_name();
  put(name.c_str(), name.size());
  put("\";oid=", sizeof("\";oid=") - 1);
  {
    char hash_buf[11];
    size_t hash_len = (size_t) snprintf(hash_buf, sizeof(hash_buf), "%u", res.entity->get_object_id_hash());
    put(hash_buf, hash_len);
  }
#ifdef USE_DEVICES
  if (res.device_index > 0) {
    char dv_buf[8];
    size_t dv_len = (size_t) snprintf(dv_buf, sizeof(dv_buf), ";dv=%u", res.device_index);
    put(dv_buf, dv_len);
  }
#endif
#ifdef USE_SENSOR
  if (res.type == ENTITYTYPE_SENSOR) {
    const auto uom_ref = static_cast<sensor::Sensor *>(res.entity)->get_unit_of_measurement_ref();
    if (uom_ref.size() > 0) {
      put(";uom=\"", sizeof(";uom=\"") - 1);
      put(uom_ref.c_str(), uom_ref.size());
      put("\"", 1);
    }
    char dc_buf[MAX_DEVICE_CLASS_LENGTH];
    const char *dc = static_cast<sensor::Sensor *>(res.entity)->get_device_class_to(dc_buf);
    size_t dc_len = strlen(dc);
    if (dc_len > 0) {
      put(";dc=\"", sizeof(";dc=\"") - 1);
      put(dc, dc_len);
      put("\"", 1);
    }
  }
#endif
#ifdef USE_TEXT_SENSOR
  if (res.type == ENTITYTYPE_TEXT_SENSOR) {
    char dc_buf[MAX_DEVICE_CLASS_LENGTH];
    const char *dc = static_cast<text_sensor::TextSensor *>(res.entity)->get_device_class_to(dc_buf);
    size_t dc_len = strlen(dc);
    if (dc_len > 0) {
      put(";dc=\"", sizeof(";dc=\"") - 1);
      put(dc, dc_len);
      put("\"", 1);
    }
  }
#endif
#ifdef USE_NUMBER
  if (res.type == ENTITYTYPE_NUMBER) {
    char dc_buf[MAX_DEVICE_CLASS_LENGTH];
    const char *dc = static_cast<number::Number *>(res.entity)->get_device_class_to(dc_buf);
    size_t dc_len = strlen(dc);
    if (dc_len > 0) {
      put(";dc=\"", sizeof(";dc=\"") - 1);
      put(dc, dc_len);
      put("\"", 1);
    }
  }
#endif
  return static_cast<uint16_t>(p - buf);
}  // format_link_entry

// static handler
void CoapServer::handle_well_known_core(void *context, otMessage *message, const otMessageInfo *message_info) {
  otError error = OT_ERROR_NONE;
  otMessage *response = nullptr;
  ehCoapResource *resource = static_cast<ehCoapResource *>(context);
  CoapServer *self = resource->server;
  otInstance *instance = self->instance_;

  // Only respond to GET requests
  if (otCoapMessageGetCode(message) != OT_COAP_CODE_GET) {
    return;
  }

  response = otCoapNewMessage(instance, nullptr);
  if (response == nullptr) {
    return;
  }
  SuccessOrExit(error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_CONTENT));
  SuccessOrExit(error = otCoapMessageAppendContentFormatOption(response, OT_COAP_OPTION_CONTENT_FORMAT_LINK_FORMAT));
  SuccessOrExit(error = otCoapMessageSetPayloadMarker(response));

  // Generate link-format payload on the fly; resources_[0] is .well-known/core itself.
  for (size_t i = 1; i < self->resources_.size(); i++) {
    char entry[256];
    uint16_t len = format_link_entry(entry, sizeof(entry), self->resources_[i], i > 1);
    SuccessOrExit(error = otMessageAppend(response, entry, len));
  }
  {
    static const char kPingEntry[] = ",</ping>;rt=\"esphome.ping\";if=\"if.a\";ct=112";
    SuccessOrExit(error = otMessageAppend(response, kPingEntry, sizeof(kPingEntry) - 1));
  }

  SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
exit:
  if (error != OT_ERROR_NONE) {
    ESP_LOGE(TAG, "coap send response error %d: %s", error, otThreadErrorToString(error));
    if (response != nullptr) {
      otMessageFree(response);
    }
  }
}  // handle_well_known_core

// static handler
void CoapServer::handle_notification_ack(void *context, otMessage *message, const otMessageInfo *message_info,
                                         otError error) {
  if (error == OT_ERROR_NONE) {
    ESP_LOGV(TAG, "Received coap notification ACK");
  } else {
    ESP_LOGI(TAG, "Notification ACK failed: %s, cancelling observer", otThreadErrorToString(error));
    ehCoapObserver *observer = static_cast<ehCoapObserver *>(context);
    CoapServer *server = nullptr;
    {
      std::lock_guard<std::mutex> lock(global_coap_server->lock_);
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

void CoapServer::handle_entity_request(ehCoapResource *resource, otMessage *message, const otMessageInfo *message_info,
                                       const EntityType type) {
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
  if (!this->oscore_unprotect_request_(message, resource, oscore_plain, &oscore_plain_len, &oscore_req_info)) {
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
  ESP_LOGI(TAG, "OSCORE handler: path=%s protected=%d plain_len=%u effective_code=0x%02x", resource->mUriPath,
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
    payload_len = cbor_output_(payload_buffer, observer != nullptr ? observer->resource : resource);
    if (payload_len == 0) {
      ESP_LOGW(TAG, "OSCORE: cbor_output_ returned 0 for path=%s type=%d", resource->mUriPath, (int) resource->type);
      SuccessOrExit(
          error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_INTERNAL_ERROR));
      SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
      goto exit;
    }
#ifdef USE_COAP_OSCORE
    if (oscore_protected) {
      // Inner: [2.05 Content][Content-Format delta=12 len=1 val=50][0xFF][payload]
      uint8_t inner[COAP_PAYLOAD_MAX_SIZE + 8];
      size_t inner_len = 0;
      inner[inner_len++] = 0x45;  // 2.05 Content
      inner[inner_len++] = 0xC1;  // Content-Format option: delta=12, len=1
      inner[inner_len++] = 0x32;  // value=50 (application/cbor)
      inner[inner_len++] = 0xFF;  // payload marker
      memcpy(inner + inner_len, payload_buffer, payload_len);
      inner_len += payload_len;
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
      CborParser parser;
      CborValue root, map_val;
      if (cbor_parser_init(msg_buf, msg_len, 0, &parser, &root) == CborNoError && cbor_value_is_map(&root) &&
          cbor_value_enter_container(&root, &map_val) == CborNoError) {
        while (!cbor_value_at_end(&map_val)) {
          int key = 0;
          bool key_ok = cbor_value_get_int(&map_val, &key) == CborNoError;
          cbor_value_advance(&map_val);
          if (cbor_value_at_end(&map_val))
            break;
          // Action By Type
          switch (type) {
#ifdef USE_SWITCH
            case EntityType::ENTITYTYPE_SWITCH:
              if (key_ok && key == 4 && cbor_value_is_boolean(&map_val)) {
                bool val = false;
                if (cbor_value_get_boolean(&map_val, &val) == CborNoError) {
                  auto *sw = static_cast<switch_::Switch *>(resource->entity);
                  if (resource->action == ACTIONTYPE_TOGGLE) {
                    if (val)
                      sw->toggle();
                  } else {
                    if (val)
                      sw->turn_on();
                    else
                      sw->turn_off();
                  }
                }
              }
              break;
#endif  // USE_SWITCH
#ifdef USE_NUMBER
            case EntityType::ENTITYTYPE_NUMBER:
              if (key_ok && key == 2) {
                double d = 0.0;
                bool got_val = false;
                if (cbor_value_is_float(&map_val) || cbor_value_is_double(&map_val) ||
                    cbor_value_is_half_float(&map_val)) {
                  got_val = cbor_value_get_double(&map_val, &d) == CborNoError;
                } else if (cbor_value_is_integer(&map_val)) {
                  int64_t i = 0;
                  if (cbor_value_get_int64(&map_val, &i) == CborNoError) {
                    d = static_cast<double>(i);
                    got_val = true;
                  }
                }
                if (got_val)
                  static_cast<number::Number *>(resource->entity)
                      ->make_call()
                      .set_value(static_cast<float>(d))
                      .perform();
              }
              break;
#endif  // USE_NUMBER
#ifdef USE_LOCK
            case EntityType::ENTITYTYPE_LOCK:
              if (key_ok && key == 4 && cbor_value_is_boolean(&map_val)) {
                bool val = false;
                if (cbor_value_get_boolean(&map_val, &val) == CborNoError) {
                  auto *lk = static_cast<lock::Lock *>(resource->entity);
                  if (val)
                    lk->lock();
                  else
                    lk->unlock();
                }
              }
              break;
#endif  // USE_LOCK
#ifdef USE_VALVE
            case EntityType::ENTITYTYPE_VALVE:
              if (key_ok && key == 4 && cbor_value_is_boolean(&map_val)) {
                bool val = false;
                if (cbor_value_get_boolean(&map_val, &val) == CborNoError) {
                  auto *vv = static_cast<valve::Valve *>(resource->entity);
                  if (resource->action == ACTIONTYPE_STOP) {
                    if (val)
                      vv->make_call().set_command_stop().perform();
                  } else {
                    if (val)
                      vv->make_call().set_command_open().perform();
                    else
                      vv->make_call().set_command_close().perform();
                  }
                }
              }
              break;
#endif  // USE_VALVE
            default:
              ESP_LOGW(TAG, "Unknown Type, no action performed");
          }
          cbor_value_advance(&map_val);
        }
      }
    }
    payload_len = cbor_output_(payload_buffer, resource);

    if (payload_len == 0) {
      SuccessOrExit(
          error = otCoapMessageInitResponse(response, message, response_type(message), OT_COAP_CODE_INTERNAL_ERROR));
      SuccessOrExit(error = otCoapSendResponse(instance, response, message_info));
      goto exit;
    }
#ifdef USE_COAP_OSCORE
    if (oscore_protected) {
      // Inner: [2.04 Changed][Content-Format delta=12 len=1 val=50][0xFF][payload]
      uint8_t inner[COAP_PAYLOAD_MAX_SIZE + 8];
      size_t inner_len = 0;
      inner[inner_len++] = 0x44;  // 2.04 Changed
      inner[inner_len++] = 0xC1;  // Content-Format option: delta=12, len=1
      inner[inner_len++] = 0x32;  // value=50 (application/cbor)
      inner[inner_len++] = 0xFF;  // payload marker
      memcpy(inner + inner_len, payload_buffer, payload_len);
      inner_len += payload_len;
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
    ESP_LOGW(TAG, "OSCORE: Method Not Allowed for path=%s effective_code=0x%02x", resource->mUriPath,
             (int) effective_code);
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
void CoapServer::handle_entity_request(void *context, otMessage *message, const otMessageInfo *message_info) {
  ehCoapResource *resource = static_cast<ehCoapResource *>(context);
  ESP_LOGV(TAG, "entity handler invoked: path=%s outer_code=0x%02x", resource->mUriPath,
           (int) otCoapMessageGetCode(message));
  resource->server->handle_entity_request(resource, message, message_info, resource->type);
}

#ifdef USE_BUTTON
// static handler
void CoapServer::handle_button_request(void *context, otMessage *message, const otMessageInfo *message_info) {
  ehCoapResource *resource = static_cast<ehCoapResource *>(context);
  ESP_LOGV(TAG, "handle_button_request");
  resource->server->handle_button_request(resource, message, message_info);
}

void CoapServer::handle_button_request(ehCoapResource *resource, otMessage *message,
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
    if (!this->oscore_unprotect_request_(message, resource, oscore_plain, &oscore_plain_len, &oscore_req_info)) {
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
void CoapServer::handle_info_request(void *context, otMessage *message, const otMessageInfo *message_info) {
  otError error = OT_ERROR_NONE;
  otMessage *response = nullptr;
  ehCoapResource *resource = static_cast<ehCoapResource *>(context);
  CoapServer *self = resource->server;
  otInstance *instance = self->instance_;

  if (otCoapMessageGetCode(message) != OT_COAP_CODE_GET)
    return;

  uint8_t payload_buf[512];
  size_t payload_len = encode_device_info_(payload_buf, sizeof(payload_buf), self);

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
void CoapServer::handle_ping_request(void *context, otMessage *message, const otMessageInfo *message_info) {
  ehCoapResource *resource = static_cast<ehCoapResource *>(context);
  CoapServer *self = resource->server;
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
  CborEncoder enc, map;
  cbor_encoder_init(&enc, payload_buf, sizeof(payload_buf), 0);
  cbor_encoder_create_map(&enc, &map, 1);
  cbor_encode_int(&map, 2);  // SenML v
  if (boot_signal) {
    cbor_encode_int(&map, -1);
  } else {
    cbor_encode_uint(&map, millis() / 1000);
  }
  cbor_encoder_close_container(&enc, &map);
  size_t payload_len = cbor_encoder_get_buffer_size(&enc, payload_buf);

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

void CoapServer::shrink_observers() {
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

void CoapServer::republish_all() {
  for (auto &res : this->resources_) {
    if (res.observable && res.entity != nullptr)
      this->on_update_(res.entity);
  }
}

#ifdef USE_SENSOR
void CoapServer::on_sensor_update(sensor::Sensor *entity) { this->on_update_(entity); }
#endif  // USE_SENSOR

#ifdef USE_SWITCH
void CoapServer::on_switch_update(switch_::Switch *entity) { this->on_update_(entity); }
#endif  // USE_SWITCH

#ifdef USE_BINARY_SENSOR
void CoapServer::on_binary_sensor_update(binary_sensor::BinarySensor *entity) { this->on_update_(entity); }
#endif  // USE_BINARY_SENSOR

#ifdef USE_TEXT_SENSOR
void CoapServer::on_text_sensor_update(text_sensor::TextSensor *entity) { this->on_update_(entity); }
#endif  // USE_TEXT_SENSOR

#ifdef USE_NUMBER
void CoapServer::on_number_update(number::Number *entity) { this->on_update_(entity); }
#endif  // USE_NUMBER

#ifdef USE_LOCK
void CoapServer::on_lock_update(lock::Lock *entity) { this->on_update_(entity); }
#endif  // USE_LOCK

#ifdef USE_VALVE
void CoapServer::on_valve_update(valve::Valve *entity) { this->on_update_(entity); }
#endif  // USE_VALVE

// Protected Methods
void CoapServer::handle_observer_(ehCoapObserver *observer, ehCoapResource *expected_resource, const uint8_t *payload,
                                  size_t payload_len) {
  // todo: don't send message while pending last ack
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
    // Build inner: [2.05 Content][Content-Format][0xFF][payload]
    uint8_t inner[COAP_PAYLOAD_MAX_SIZE + 8];
    size_t inner_len = 0;
    inner[inner_len++] = 0x45;  // 2.05 Content
    inner[inner_len++] = 0xC1;  // Content-Format delta=12, len=1
    inner[inner_len++] = 0x32;  // value=50 (application/cbor)
    inner[inner_len++] = 0xFF;
    memcpy(inner + inner_len, payload, payload_len);
    inner_len += payload_len;
    // For notifications, req_info.piv/kid are unused (notification nonce uses sender_seq_no_ and sender_id)
    OscoreRequestInfo dummy{};
    uint8_t ciphertext[COAP_PAYLOAD_MAX_SIZE + OSCORE_TAG_LEN + 8];
    size_t cipher_len = this->oscore_protect_response_(inner, inner_len, dummy, true, ciphertext, sizeof(ciphertext));
    if (cipher_len == 0) {
      error = OT_ERROR_FAILED;
      goto exit;
    }
    // Notification OSCORE option: flags = 0x08 (kid present) | piv_len
    // Build option value: [flags][piv bytes][kid bytes]
    uint32_t seq_for_opt = (this->oscore_sender_seq_no_ == 0) ? 0 : (this->oscore_sender_seq_no_ - 1);
    uint8_t piv_len_opt = (seq_for_opt <= 0xFF) ? 1 : (seq_for_opt <= 0xFFFF) ? 2 : 3;
    uint8_t oscore_opt[16];
    uint8_t opt_pos = 0;
    oscore_opt[opt_pos++] = (uint8_t) (0x08 | piv_len_opt);  // h=0, k=1, n=piv_len
    if (piv_len_opt == 3)
      oscore_opt[opt_pos++] = (uint8_t) (seq_for_opt >> 16);
    if (piv_len_opt >= 2)
      oscore_opt[opt_pos++] = (uint8_t) (seq_for_opt >> 8);
    oscore_opt[opt_pos++] = (uint8_t) seq_for_opt;
    memcpy(oscore_opt + opt_pos, this->oscore_sender_id_buf_, this->oscore_sender_id_len_);
    opt_pos += this->oscore_sender_id_len_;
    SuccessOrExit(error = otCoapMessageAppendOption(message, 9, opt_pos, oscore_opt));
    SuccessOrExit(error = otCoapMessageSetPayloadMarker(message));
    SuccessOrExit(error = otMessageAppend(message, ciphertext, (uint16_t) cipher_len));
    SuccessOrExit(error = otCoapSendRequest(instance, message, &msg_info_copy,
                                            is_con ? CoapServer::handle_notification_ack : nullptr, observer));
  }
#else
  SuccessOrExit(error = otCoapMessageAppendContentFormatOption(message, OT_COAP_OPTION_CONTENT_FORMAT_CBOR));
  SuccessOrExit(error = otCoapMessageSetPayloadMarker(message));
  SuccessOrExit(error = otMessageAppend(message, payload, (uint16_t) payload_len));
  SuccessOrExit(error = otCoapSendRequest(instance, message, &msg_info_copy,
                                          is_con ? CoapServer::handle_notification_ack : nullptr, observer));
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
static void append_uint16_decimal(char *buf, uint8_t &pos, uint16_t n) {
  if (n == 0) {
    buf[pos++] = '0';
    return;
  }
  uint8_t start = pos;
  while (n > 0) {
    buf[pos++] = '0' + (n % 10);
    n /= 10;
  }
  for (uint8_t i = start, j = pos - 1; i < j; i++, j--) {
    char t = buf[i];
    buf[i] = buf[j];
    buf[j] = t;
  }
}

void CoapServer::add_coap_resource_(EntityType type, EntityBase *entity, bool observable, uint16_t &senml_index) {
  auto set_domain = []<size_t N>(char(&dest)[sizeof(ehCoapResource::domain)], const char(&src)[N]) {
    static_assert(N < sizeof(dest), "domain name too long for field");
    memcpy(dest, src, N);
  };
  this->resources_.push_back(ehCoapResource());
  ehCoapResource *resource = &(this->resources_[this->resources_.size() - 1]);

  switch (type) {
#ifdef USE_SENSOR
    case EntityType::ENTITYTYPE_SENSOR:
      set_domain(resource->domain, "sensor");
      break;
#endif  // USE_SENSOR
#ifdef USE_SWITCH
    case EntityType::ENTITYTYPE_SWITCH:
      set_domain(resource->domain, "switch");
      break;
#endif  // USE_SWITCH
#ifdef USE_BINARY_SENSOR
    case EntityType::ENTITYTYPE_BINARY_SENSOR:
      set_domain(resource->domain, "binary_sensor");
      break;
#endif  // USE_BINARY_SENSOR
#ifdef USE_BUTTON
    case EntityType::ENTITYTYPE_BUTTON:
      set_domain(resource->domain, "button");
      break;
#endif  // USE_BUTTON
#ifdef USE_TEXT_SENSOR
    case EntityType::ENTITYTYPE_TEXT_SENSOR:
      set_domain(resource->domain, "text_sensor");
      break;
#endif  // USE_TEXT_SENSOR
#ifdef USE_NUMBER
    case EntityType::ENTITYTYPE_NUMBER:
      set_domain(resource->domain, "number");
      break;
#endif  // USE_NUMBER
#ifdef USE_LOCK
    case EntityType::ENTITYTYPE_LOCK:
      set_domain(resource->domain, "lock");
      break;
#endif  // USE_LOCK
#ifdef USE_VALVE
    case EntityType::ENTITYTYPE_VALVE:
      set_domain(resource->domain, "valve");
      break;
#endif  // USE_VALVE
#ifdef USE_LOGGER
    case EntityType::ENTITYTYPE_LOG:
      set_domain(resource->domain, "log");
      break;
#endif  // USE_LOGGER
    case EntityType::ENTITYTYPE_UNKNOWN:
    default:
      set_domain(resource->domain, "unknown");
  }
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
#ifdef USE_DEVICES
  if (entity != nullptr && entity->get_device_id() != 0) {
    uint8_t dev_idx = 1;
    for (Device *dev : App.get_devices()) {
      if (dev->get_device_id() == entity->get_device_id()) {
        resource->device_index = dev_idx;
        break;
      }
      dev_idx++;
    }
  }
#endif
  resource->type = type;
  resource->mUriPath = resource->path;

  resource->mHandler = &CoapServer::handle_entity_request;
#ifdef USE_BUTTON
  if (type == ENTITYTYPE_BUTTON)
    resource->mHandler = &CoapServer::handle_button_request;
#endif
#ifdef USE_LOGGER
  if (type == ENTITYTYPE_LOG)
    resource->mHandler = &CoapServer::handle_logs_request;
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
    toggle_resource->device_index = resource->device_index;
    toggle_resource->type = type;
    toggle_resource->action = ACTIONTYPE_TOGGLE;
    toggle_resource->mUriPath = toggle_resource->path;
    toggle_resource->mHandler = &CoapServer::handle_entity_request;
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
    stop_resource->device_index = resource->device_index;
    stop_resource->type = type;
    stop_resource->action = ACTIONTYPE_STOP;
    stop_resource->mUriPath = stop_resource->path;
    stop_resource->mHandler = &CoapServer::handle_entity_request;
    stop_resource->mContext = stop_resource;
    strncpy(stop_resource->domain, resource->domain, sizeof(stop_resource->domain));
    otCoapAddResource(this->instance_, stop_resource);
    ESP_LOGD(TAG, "Add CoAP Server Resource: /%s", stop_resource->mUriPath);
  }
#endif
}

uint8_t CoapServer::observe_(otMessage *message) {
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

ehCoapObserver *CoapServer::get_observer_(otMessage *message, const otMessageInfo *message_info) {
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

ehCoapObserver *CoapServer::new_observer_(ehCoapResource *resource, const otMessageInfo &message_info,
                                          const otCoapToken &token, otCoapType obs_type) {
  std::lock_guard<std::mutex> lock(this->lock_);
  ehCoapObserver *obs;
  if (this->free_observers_ != nullptr) {
    obs = this->free_observers_;
    this->free_observers_ = obs->next;
  } else {
    obs = new ehCoapObserver();
  }
  obs->resource = resource;
  obs->message_info = message_info;
  obs->coap_token = token;
  obs->observe_serial = 0;
  obs->obs_type = obs_type;
  obs->notify_count = 0;
  // Prepend to active list only after full initialisation so on_update_ never
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

ehCoapClient *CoapServer::new_client_(const otMessageInfo &message_info) {
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

void CoapServer::ping_client_(ehCoapClient *client) {
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
              std::lock_guard<std::mutex> guard(global_coap_server->lock_);
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

void CoapServer::cancel_ping_client_(ehCoapClient *client) {
  char key[] = "client_ping_0";
  key[sizeof("client_ping_") - 1] = '0' + client->slot;
  this->cancel_timeout(key);
}

ehCoapClient *CoapServer::find_client_(const otIp6Address &addr) {
  for (auto &client : this->active_clients_) {
    if (client.active && otIp6IsAddressEqual(&client.peer_addr, &addr))
      return &client;
  }
  return nullptr;
}

void CoapServer::touch_client_(const otMessageInfo &message_info) {
  std::lock_guard<std::mutex> guard(this->lock_);
  ehCoapClient *client = this->find_client_(message_info.mPeerAddr);
  if (client != nullptr) {
    client->last_response_ms = millis();
    client->ping_miss_count = 0;
  }
}

void CoapServer::free_client_(ehCoapClient *client) {
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

void CoapServer::free_observer_(ehCoapObserver *observer) {
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

void CoapServer::notify_observers_(ehCoapResource *resource, const uint8_t *payload, size_t payload_len) {
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

void CoapServer::on_update_(EntityBase *entity) {
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
  size_t payload_len = cbor_output_(payload_buffer, resource);
  if (payload_len == 0) {
    ESP_LOGW(TAG, "cbor encode failed, skip notifications");
    return;
  }
  this->notify_observers_(resource, payload_buffer, payload_len);
}

#ifdef USE_COAP_OSCORE

// Encode the OSCORE HKDF info array per RFC 8613 §3.2.1:
// [id (bstr), id_context (bstr / null), alg (int = 10), type (tstr), L (uint)]
static size_t oscore_build_info_(uint8_t *buf, size_t buf_len, const uint8_t *id, size_t id_len,
                                 const uint8_t *id_context, size_t id_context_len, const char *type, uint8_t L) {
  static const uint8_t kEmpty[1] = {};
  CborEncoder enc, arr;
  cbor_encoder_init(&enc, buf, buf_len, 0);
  if (cbor_encoder_create_array(&enc, &arr, 5) != CborNoError)
    return 0;
  if (cbor_encode_byte_string(&arr, id_len > 0 ? id : kEmpty, id_len) != CborNoError)
    return 0;
  if (id_context_len > 0) {
    if (cbor_encode_byte_string(&arr, id_context, id_context_len) != CborNoError)
      return 0;
  } else {
    if (cbor_encode_null(&arr) != CborNoError)
      return 0;
  }
  if (cbor_encode_int(&arr, 10) != CborNoError)  // AEAD_AES_128_CCM
    return 0;
  if (cbor_encode_text_stringz(&arr, type) != CborNoError)
    return 0;
  if (cbor_encode_uint(&arr, L) != CborNoError)
    return 0;
  if (cbor_encoder_close_container(&enc, &arr) != CborNoError)
    return 0;
  return cbor_encoder_get_buffer_size(&enc, buf);
}

// PSA Crypto is already initialised by OpenThread; no psa_crypto_init() needed here.
static bool oscore_hkdf_(const uint8_t *secret, size_t secret_len, const uint8_t *salt, size_t salt_len,
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

bool CoapServer::oscore_derive_keys_() {
  uint8_t info_buf[64];
  size_t info_len;
  uint8_t key_buf[OSCORE_KEY_LEN];

  // Sender key — derive and store raw bytes; imported as transient PSA key per encrypt call
  info_len =
      oscore_build_info_(info_buf, sizeof(info_buf), this->oscore_sender_id_.data(), this->oscore_sender_id_.size(),
                         this->oscore_id_context_.data(), this->oscore_id_context_.size(), "Key", OSCORE_KEY_LEN);
  if (info_len == 0 || !oscore_hkdf_(this->oscore_master_secret_.data(), this->oscore_master_secret_.size(),
                                     this->oscore_master_salt_.data(), this->oscore_master_salt_.size(), info_buf,
                                     info_len, key_buf, OSCORE_KEY_LEN)) {
    ESP_LOGE(TAG, "OSCORE sender key derivation failed");
    return false;
  }
  memcpy(this->oscore_sender_key_, key_buf, OSCORE_KEY_LEN);
  memset(key_buf, 0, sizeof(key_buf));

  // Recipient key — derive and store raw bytes; imported as transient PSA key per decrypt call
  info_len = oscore_build_info_(info_buf, sizeof(info_buf), this->oscore_recipient_id_.data(),
                                this->oscore_recipient_id_.size(), this->oscore_id_context_.data(),
                                this->oscore_id_context_.size(), "Key", OSCORE_KEY_LEN);
  if (info_len == 0 || !oscore_hkdf_(this->oscore_master_secret_.data(), this->oscore_master_secret_.size(),
                                     this->oscore_master_salt_.data(), this->oscore_master_salt_.size(), info_buf,
                                     info_len, key_buf, OSCORE_KEY_LEN)) {
    ESP_LOGE(TAG, "OSCORE recipient key derivation failed");
    return false;
  }
  memcpy(this->oscore_recipient_key_, key_buf, OSCORE_KEY_LEN);
  memset(key_buf, 0, sizeof(key_buf));

  // Common IV — stored as raw bytes for nonce construction (not an AEAD key)
  info_len = oscore_build_info_(info_buf, sizeof(info_buf), nullptr, 0, this->oscore_id_context_.data(),
                                this->oscore_id_context_.size(), "IV", OSCORE_IV_LEN);
  if (info_len == 0 || !oscore_hkdf_(this->oscore_master_secret_.data(), this->oscore_master_secret_.size(),
                                     this->oscore_master_salt_.data(), this->oscore_master_salt_.size(), info_buf,
                                     info_len, this->oscore_common_iv_, OSCORE_IV_LEN)) {
    ESP_LOGE(TAG, "OSCORE common IV derivation failed");
    return false;
  }

  ESP_LOGI(TAG, "OSCORE keys derived");

  // Retain sender ID (needed for notification OSCORE option) then release all key material.
  this->oscore_sender_id_len_ = (uint8_t) std::min(this->oscore_sender_id_.size(), sizeof(this->oscore_sender_id_buf_));
  memcpy(this->oscore_sender_id_buf_, this->oscore_sender_id_.data(), this->oscore_sender_id_len_);

  this->oscore_master_secret_.clear();
  this->oscore_master_secret_.shrink_to_fit();
  this->oscore_master_salt_.clear();
  this->oscore_master_salt_.shrink_to_fit();
  this->oscore_sender_id_.clear();
  this->oscore_sender_id_.shrink_to_fit();
  this->oscore_recipient_id_.clear();
  this->oscore_recipient_id_.shrink_to_fit();
  this->oscore_id_context_.clear();
  this->oscore_id_context_.shrink_to_fit();

  return true;
}

// Nonce = [kid_len(1) | padded_kid(nonce_len-6) | padded_piv(5)] XOR common_iv
void CoapServer::oscore_build_nonce_(const uint8_t *piv, uint8_t piv_len, const uint8_t *kid, uint8_t kid_len,
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
size_t CoapServer::oscore_build_aad_(const uint8_t *kid, uint8_t kid_len, const uint8_t *piv, uint8_t piv_len,
                                     uint8_t *buf, size_t buf_len) {
  // Build inner aad_array first into a temp buffer
  uint8_t aad_array_buf[32];
  CborEncoder enc, arr;
  cbor_encoder_init(&enc, aad_array_buf, sizeof(aad_array_buf), 0);
  if (cbor_encoder_create_array(&enc, &arr, 5) != CborNoError)
    return 0;
  if (cbor_encode_uint(&arr, 1) != CborNoError)  // oscore_version
    return 0;
  CborEncoder alg_arr;
  if (cbor_encoder_create_array(&arr, &alg_arr, 1) != CborNoError)
    return 0;
  if (cbor_encode_int(&alg_arr, 10) != CborNoError)  // AES-CCM-16-64-128
    return 0;
  if (cbor_encoder_close_container(&arr, &alg_arr) != CborNoError)
    return 0;
  if (cbor_encode_byte_string(&arr, kid, kid_len) != CborNoError)
    return 0;
  if (cbor_encode_byte_string(&arr, piv, piv_len) != CborNoError)
    return 0;
  static const uint8_t kEmpty[1] = {};
  if (cbor_encode_byte_string(&arr, kEmpty, 0) != CborNoError)  // class-I options (empty)
    return 0;
  if (cbor_encoder_close_container(&enc, &arr) != CborNoError)
    return 0;
  size_t aad_array_len = cbor_encoder_get_buffer_size(&enc, aad_array_buf);

  // Enc_Structure = ["Encrypt0", h'', aad_array_bstr]
  CborEncoder outer, outer_arr;
  cbor_encoder_init(&outer, buf, buf_len, 0);
  if (cbor_encoder_create_array(&outer, &outer_arr, 3) != CborNoError)
    return 0;
  if (cbor_encode_text_stringz(&outer_arr, "Encrypt0") != CborNoError)
    return 0;
  if (cbor_encode_byte_string(&outer_arr, kEmpty, 0) != CborNoError)
    return 0;
  if (cbor_encode_byte_string(&outer_arr, aad_array_buf, aad_array_len) != CborNoError)
    return 0;
  if (cbor_encoder_close_container(&outer, &outer_arr) != CborNoError)
    return 0;
  return cbor_encoder_get_buffer_size(&outer, buf);
}

// Parse incoming message for OSCORE option, decrypt, return inner CoAP bytes.
// Returns true (and empty plaintext_len) for exempt resources without checking.
// Returns false and logs if OSCORE required but absent or decryption fails.
bool CoapServer::oscore_unprotect_request_(otMessage *message, const ehCoapResource *resource, uint8_t *plaintext,
                                           size_t *plaintext_len, OscoreRequestInfo *req_info) {
  *plaintext_len = 0;
  if (resource->oscore_exempt)
    return true;

  // Find OSCORE option (option number 9)
  otCoapOptionIterator iter;
  otCoapOptionIteratorInit(&iter, message);
  const otCoapOption *oscore_opt = otCoapOptionIteratorGetFirstOptionMatching(&iter, 9);
  if (oscore_opt == nullptr) {
    ESP_LOGW(TAG, "OSCORE: request on protected resource /%s without OSCORE option", resource->mUriPath);
    return false;
  }

  // Parse OSCORE option value
  uint8_t opt_val[32];
  uint16_t opt_len = oscore_opt->mLength;
  if (opt_len > sizeof(opt_val)) {
    ESP_LOGW(TAG, "OSCORE: option value too long (%u bytes)", opt_len);
    return false;
  }
  if (opt_len > 0)
    otCoapOptionIteratorGetOptionValue(&iter, opt_val);

  uint8_t flags = (opt_len > 0) ? opt_val[0] : 0;
  uint8_t piv_len = flags & 0x07;
  bool has_kid_ctx = (flags >> 4) & 1;  // RFC 8613 Table 1: bit 4 = h (KID Context flag)
  bool has_kid = (flags >> 3) & 1;      // RFC 8613 Table 1: bit 3 = k (KID flag)

  uint8_t pos = (opt_len > 0) ? 1u : 0u;
  // Partial IV
  if (piv_len > 0) {
    if (pos + piv_len > opt_len) {
      ESP_LOGW(TAG, "OSCORE: PIV overflow pos=%u piv_len=%u opt_len=%u", pos, piv_len, opt_len);
      return false;
    }
    memcpy(req_info->piv, opt_val + pos, piv_len);
    pos += piv_len;
  }
  req_info->piv_len = piv_len;
  // KID Context (skip)
  if (has_kid_ctx) {
    if (pos >= opt_len) {
      ESP_LOGW(TAG, "OSCORE: KID context overflow pos=%u opt_len=%u", pos, opt_len);
      return false;
    }
    uint8_t ctx_len = opt_val[pos++];
    pos += ctx_len;
  }
  // KID (sender ID from client)
  req_info->kid_len = 0;
  if (has_kid) {
    uint8_t kid_len = (uint8_t) (opt_len - pos);
    if (kid_len > sizeof(req_info->kid))
      return false;
    memcpy(req_info->kid, opt_val + pos, kid_len);
    req_info->kid_len = kid_len;
  }

  // Sliding-window replay check (64-entry, RFC 8613 §7.4).
  // Window is updated only after successful AEAD decryption to prevent poisoning
  // via forged packets with high sequence numbers.
  uint32_t seq = 0;
  for (uint8_t i = 0; i < piv_len; i++)
    seq = (seq << 8) | req_info->piv[i];
  if (piv_len > 0 && this->oscore_replay_mask_ != 0) {
    if (seq <= this->oscore_replay_top_) {
      uint32_t offset = this->oscore_replay_top_ - seq;
      if (offset >= 64 || (this->oscore_replay_mask_ & (1ULL << offset))) {
        ESP_LOGW(TAG, "OSCORE: replayed sequence number %" PRIu32, seq);
        return false;
      }
    }
  }

  // Build nonce and AAD
  uint8_t nonce[OSCORE_IV_LEN];
  oscore_build_nonce_(req_info->piv, piv_len, req_info->kid, req_info->kid_len, this->oscore_common_iv_, nonce);
  uint8_t aad_buf[80];
  size_t aad_len =
      oscore_build_aad_(req_info->kid, req_info->kid_len, req_info->piv, piv_len, aad_buf, sizeof(aad_buf));
  if (aad_len == 0) {
    ESP_LOGE(TAG, "OSCORE: AAD build failed");
    return false;
  }

  // Read ciphertext from message payload
  uint16_t msg_offset = otMessageGetOffset(message);
  uint16_t ciphertext_len = otMessageGetLength(message) - msg_offset;
  if (ciphertext_len == 0 || ciphertext_len > 256) {
    ESP_LOGW(TAG, "OSCORE: unexpected ciphertext length %u", ciphertext_len);
    return false;
  }
  uint8_t ciphertext[256];
  otMessageRead(message, msg_offset, ciphertext, ciphertext_len);

  // Decrypt — import transient key, use once, destroy immediately (avoids PSA context resets by OpenThread)
  ESP_LOGV(TAG, "OSCORE decrypt: piv_len=%u kid_len=%u cipher_len=%u", piv_len, req_info->kid_len,
           (unsigned) ciphertext_len);
  ESP_LOG_BUFFER_HEX_LEVEL(TAG, req_info->piv, piv_len, ESP_LOG_VERBOSE);
  ESP_LOGV(TAG, "OSCORE kid:");
  ESP_LOG_BUFFER_HEX_LEVEL(TAG, req_info->kid, req_info->kid_len, ESP_LOG_VERBOSE);
  ESP_LOGV(TAG, "OSCORE nonce:");
  ESP_LOG_BUFFER_HEX_LEVEL(TAG, nonce, OSCORE_IV_LEN, ESP_LOG_VERBOSE);
  ESP_LOGV(TAG, "OSCORE aad (%u bytes):", (unsigned) aad_len);
  ESP_LOG_BUFFER_HEX_LEVEL(TAG, aad_buf, aad_len, ESP_LOG_VERBOSE);
  ESP_LOGV(TAG, "OSCORE recipient_key:");
  ESP_LOG_BUFFER_HEX_LEVEL(TAG, this->oscore_recipient_key_, OSCORE_KEY_LEN, ESP_LOG_VERBOSE);
  size_t out_len = 0;
  {
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attrs, 128);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attrs, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, OSCORE_TAG_LEN));
    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    psa_status_t s = psa_import_key(&attrs, this->oscore_recipient_key_, OSCORE_KEY_LEN, &key_id);
    if (s != PSA_SUCCESS) {
      ESP_LOGE(TAG, "OSCORE: recipient key import failed: %d", (int) s);
      return false;
    }
    s = psa_aead_decrypt(key_id, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, OSCORE_TAG_LEN), nonce, OSCORE_IV_LEN,
                         aad_buf, aad_len, ciphertext, ciphertext_len, plaintext, 256, &out_len);
    psa_destroy_key(key_id);
    if (s != PSA_SUCCESS) {
      ESP_LOGW(TAG, "OSCORE: decryption failed: %d", (int) s);
      return false;
    }
  }

  if (piv_len > 0) {
    if (this->oscore_replay_mask_ == 0 || seq > this->oscore_replay_top_) {
      if (this->oscore_replay_mask_ != 0) {
        uint32_t shift = seq - this->oscore_replay_top_;
        this->oscore_replay_mask_ = (shift < 64) ? (this->oscore_replay_mask_ << shift) : 0;
      }
      this->oscore_replay_top_ = seq;
      this->oscore_replay_mask_ |= 1ULL;
    } else {
      uint32_t offset = this->oscore_replay_top_ - seq;
      this->oscore_replay_mask_ |= (1ULL << offset);
    }
  }
  *plaintext_len = out_len;
  ESP_LOGD(TAG, "OSCORE: request decrypted (seq=%" PRIu32 " plaintext=%u bytes)", seq, (unsigned) out_len);
  return true;
}

// Encrypt inner CoAP bytes (code + options + payload) into out_buf.
// Returns 0 on failure. For notifications, advances sender_seq_no_.
size_t CoapServer::oscore_protect_response_(const uint8_t *inner, size_t inner_len, const OscoreRequestInfo &req_info,
                                            bool is_notification, uint8_t *out_buf, size_t out_buf_len) {
  uint8_t piv[5];
  uint8_t piv_len = 0;
  uint8_t kid[8];
  uint8_t kid_len = 0;

  if (is_notification) {
    // Use server's sequence number as PIV; encode as minimal big-endian bytes
    uint32_t seq = this->oscore_sender_seq_no_;
    if (seq <= 0xFF) {
      piv[0] = (uint8_t) seq;
      piv_len = 1;
    } else if (seq <= 0xFFFF) {
      piv[0] = (uint8_t) (seq >> 8);
      piv[1] = (uint8_t) seq;
      piv_len = 2;
    } else {
      piv[0] = (uint8_t) (seq >> 16);
      piv[1] = (uint8_t) (seq >> 8);
      piv[2] = (uint8_t) seq;
      piv_len = 3;
    }
    // Notification nonce uses server's sender_id as KID
    memcpy(kid, this->oscore_sender_id_buf_, this->oscore_sender_id_len_);
    kid_len = this->oscore_sender_id_len_;
    this->oscore_increment_seq_no_();
  } else {
    // Simple response: reuse the request's PIV and KID for nonce (RFC 8613 §8.3)
    memcpy(piv, req_info.piv, req_info.piv_len);
    piv_len = req_info.piv_len;
    memcpy(kid, req_info.kid, req_info.kid_len);
    kid_len = req_info.kid_len;
  }

  uint8_t nonce[OSCORE_IV_LEN];
  oscore_build_nonce_(piv, piv_len, kid, kid_len, this->oscore_common_iv_, nonce);
  uint8_t aad_buf[80];
  size_t aad_len =
      oscore_build_aad_(req_info.kid, req_info.kid_len, req_info.piv, req_info.piv_len, aad_buf, sizeof(aad_buf));
  if (aad_len == 0)
    return 0;

  // Encrypt — import transient key, use once, destroy immediately
  size_t out_len = 0;
  {
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attrs, 128);
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_ENCRYPT);
    psa_set_key_algorithm(&attrs, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, OSCORE_TAG_LEN));
    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    psa_status_t s = psa_import_key(&attrs, this->oscore_sender_key_, OSCORE_KEY_LEN, &key_id);
    if (s != PSA_SUCCESS) {
      ESP_LOGE(TAG, "OSCORE: sender key import failed: %d", (int) s);
      return 0;
    }
    s = psa_aead_encrypt(key_id, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, OSCORE_TAG_LEN), nonce, OSCORE_IV_LEN,
                         aad_buf, aad_len, inner, inner_len, out_buf, out_buf_len, &out_len);
    psa_destroy_key(key_id);
    if (s != PSA_SUCCESS) {
      ESP_LOGE(TAG, "OSCORE: encryption failed: %d", (int) s);
      return 0;
    }
  }
  ESP_LOGD(TAG, "OSCORE: response encrypted (%s ciphertext=%u bytes)", is_notification ? "notification" : "reply",
           (unsigned) out_len);
  return out_len;
}

void CoapServer::oscore_save_seq_no_() {
  nvs_handle_t handle;
  if (nvs_open("coap_oscore", NVS_READWRITE, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "OSCORE: failed to open NVS for sequence number save");
    return;
  }
  this->oscore_seq_threshold_ = this->oscore_sender_seq_no_ + OSCORE_SEQ_INTERVAL;
  nvs_set_u32(handle, "seq", this->oscore_seq_threshold_);
  nvs_commit(handle);
  nvs_close(handle);
}

void CoapServer::oscore_increment_seq_no_() {
  this->oscore_sender_seq_no_++;
  if (this->oscore_sender_seq_no_ >= this->oscore_seq_threshold_)
    this->oscore_save_seq_no_();
}

#endif  // USE_COAP_OSCORE

#ifdef USE_LOGGER

// static
void CoapServer::log_callback_(void *self, uint8_t level, const char *tag, const char *message, size_t message_len) {
  static_cast<CoapServer *>(self)->on_log_(level, tag, message, message_len);
}

// static handler
void CoapServer::handle_logs_request(void *context, otMessage *message, const otMessageInfo *message_info) {
  ehCoapResource *resource = static_cast<ehCoapResource *>(context);
  CoapServer *self = resource->server;
  otInstance *instance = self->instance_;

  otError error = OT_ERROR_NONE;
  otMessage *response = nullptr;

  if (otCoapMessageGetCode(message) != OT_COAP_CODE_GET)
    return;

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

void CoapServer::on_log_(uint8_t level, const char *tag, const char *message, size_t message_len) {
  // Skip if no observer is watching — check under lock_ to avoid data race
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

  // Encode [millis, level, tag, message] into a temp buffer
  uint8_t entry_buf[320];
  CborEncoder enc, arr;
  cbor_encoder_init(&enc, entry_buf, sizeof(entry_buf), 0);
  cbor_encoder_create_array(&enc, &arr, 4);
  cbor_encode_uint(&arr, millis());
  cbor_encode_uint(&arr, level);
  cbor_encode_text_stringz(&arr, tag != nullptr ? tag : "");
  cbor_encode_text_string(&arr, message, message_len);
  cbor_encoder_close_container(&enc, &arr);
  size_t entry_size = cbor_encoder_get_buffer_size(&enc, entry_buf);
  if (entry_size > sizeof(entry_buf))
    return;  // entry too large to encode; drop it

  std::lock_guard<std::mutex> guard(this->log_mutex_);
  // Need room for the entry plus the 1-byte CBOR break that closes the outer array
  if (this->log_buf_pos_ + entry_size + 1 > LOG_BUF_SIZE)
    return;  // buffer full; entry dropped — will flush shortly
  memcpy(this->log_buf_ + this->log_buf_pos_, entry_buf, entry_size);
  this->log_buf_pos_ += entry_size;
  this->log_buf_has_data_ = true;
}

void CoapServer::flush_logs_() {
  uint8_t payload[LOG_BUF_SIZE];
  size_t payload_len = 0;
  {
    std::lock_guard<std::mutex> guard(this->log_mutex_);
    if (this->log_buf_has_data_) {
      this->log_buf_[this->log_buf_pos_] = 0xFF;  // CBOR break byte — closes the indefinite array
      payload_len = this->log_buf_pos_ + 1;
      memcpy(payload, this->log_buf_, payload_len);
      this->log_buf_[0] = 0x9F;
      this->log_buf_pos_ = 1;
      this->log_buf_has_data_ = false;
    }
  }
  if (payload_len > 0)
    this->notify_observers_(this->logs_resource_, payload, payload_len);

  // otLinkGetPollPeriod is a read-only getter with no state mutation; safe to call without InstanceLock.
  uint32_t flush_ms = std::max(otLinkGetPollPeriod(this->instance_), (uint32_t) 1000);
  this->set_timeout("log_flush", flush_ms, [this]() { this->flush_logs_(); });
}

#endif  // USE_LOGGER

}  // namespace esphome::coap_server
#endif  // USE_OPENTHREAD
