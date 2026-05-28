#include "coap_server.h"
#ifndef USE_OPENTHREAD
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "cbor.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#ifdef USE_COAP_OSCORE
#include "nvs.h"
#include <psa/crypto.h>
#endif
#ifdef USE_LOGGER
#include "esphome/components/logger/logger.h"
#endif

namespace esphome::coap_server {

static const char *const TAG = "coap_server";

// Response type: CON(0)→ACK(2), NON(1)→NON(1)
static inline uint8_t resp_type(uint8_t req_type) { return (req_type == 0) ? 2 : 1; }

// ---------------------------------------------------------------------------
// Link-format entry
// ---------------------------------------------------------------------------

static bool addr_equal(const sockaddr_in6 &a, const sockaddr_in6 &b) {
  return memcmp(&a.sin6_addr, &b.sin6_addr, sizeof(a.sin6_addr)) == 0 && a.sin6_port == b.sin6_port;
}

static void addr_to_str(const sockaddr_in6 &addr, char *buf, size_t len) {
  inet_ntop(AF_INET6, &addr.sin6_addr, buf, (socklen_t) len);
}

// ---------------------------------------------------------------------------
// setup / loop / teardown / dump_config
// ---------------------------------------------------------------------------

void CoapServerNet::setup() {
  CoapServer::setup();
  if (this->is_failed())
    return;

  this->sock_ = socket(AF_INET6, SOCK_DGRAM, 0);
  if (this->sock_ < 0) {
    ESP_LOGE(TAG, "CoAP Net: socket() failed: %d", errno);
    this->mark_failed();
    return;
  }

  int yes = 1;
  setsockopt(this->sock_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in6 bind_addr{};
  bind_addr.sin6_family = AF_INET6;
  bind_addr.sin6_port = htons(USE_COAP_SERVER_PORT);
  bind_addr.sin6_addr = in6addr_any;
  if (bind(this->sock_, (struct sockaddr *) &bind_addr, sizeof(bind_addr)) < 0) {
    ESP_LOGE(TAG, "CoAP Net: bind() failed: %d", errno);
    close(this->sock_);
    this->sock_ = -1;
    this->mark_failed();
    return;
  }

  fcntl(this->sock_, F_SETFL, fcntl(this->sock_, F_GETFL, 0) | O_NONBLOCK);
  ESP_LOGI(TAG, "CoAP Net server started on port %u", USE_COAP_SERVER_PORT);

  this->resources_.init(CoapServer::count_resources());

  // .well-known/core
  this->resources_.push_back(NetCoapResource());
  {
    auto &r = this->resources_[this->resources_.size() - 1];
    strncpy(r.path, ".well-known/core", sizeof(r.path));
    r.oscore_exempt = true;
  }

  // /info
  this->resources_.push_back(NetCoapResource());
  {
    auto &r = this->resources_[this->resources_.size() - 1];
    strncpy(r.path, "info", sizeof(r.path));
    strncpy(r.domain, "device", sizeof(r.domain));
    r.type = ENTITYTYPE_DEVICE;
    r.oscore_exempt = true;
  }

  uint16_t senml_index = 1;
#ifdef USE_BINARY_SENSOR
  for (auto *e : App.get_binary_sensors())
    if (!e->is_internal())
      add_net_resource_(ENTITYTYPE_BINARY_SENSOR, e, true, senml_index);
#endif
#ifdef USE_BUTTON
  for (auto *e : App.get_buttons())
    if (!e->is_internal())
      add_net_resource_(ENTITYTYPE_BUTTON, e, false, senml_index);
#endif
#ifdef USE_LOCK
  for (auto *e : App.get_locks())
    if (!e->is_internal())
      add_net_resource_(ENTITYTYPE_LOCK, e, true, senml_index);
#endif
#ifdef USE_NUMBER
  for (auto *e : App.get_numbers())
    if (!e->is_internal())
      add_net_resource_(ENTITYTYPE_NUMBER, e, true, senml_index);
#endif
#ifdef USE_SENSOR
  for (auto *e : App.get_sensors())
    if (!e->is_internal())
      add_net_resource_(ENTITYTYPE_SENSOR, e, true, senml_index);
#endif
#ifdef USE_SWITCH
  for (auto *e : App.get_switches())
    if (!e->is_internal())
      add_net_resource_(ENTITYTYPE_SWITCH, e, true, senml_index);
#endif
#ifdef USE_TEXT_SENSOR
  for (auto *e : App.get_text_sensors())
    if (!e->is_internal())
      add_net_resource_(ENTITYTYPE_TEXT_SENSOR, e, true, senml_index);
#endif
#ifdef USE_VALVE
  for (auto *e : App.get_valves())
    if (!e->is_internal())
      add_net_resource_(ENTITYTYPE_VALVE, e, true, senml_index);
#endif
#ifdef USE_LOGGER
  add_net_resource_(ENTITYTYPE_LOG, nullptr, true, senml_index);
  this->logs_resource_ = &this->resources_[this->resources_.size() - 1];
  this->log_buf_[0] = 0x9F;
  this->log_buf_pos_ = 1;
  if (logger::global_logger != nullptr)
    logger::global_logger->add_log_callback(this, CoapServer::log_callback);
  this->set_timeout("log_flush", 1000, [this]() { this->flush_logs_(); });
#endif
}

void CoapServerNet::loop() {
  if (this->sock_ < 0)
    return;
  sockaddr_in6 peer{};
  socklen_t peer_len = sizeof(peer);
  ssize_t n;
  while ((n = recvfrom(this->sock_, this->recv_buf_, sizeof(this->recv_buf_), 0, (struct sockaddr *) &peer,
                       &peer_len)) > 0)
    this->process_datagram_(this->recv_buf_, (size_t) n, &peer);
}

bool CoapServerNet::teardown() {
  for (auto *obs = this->active_observers_; obs != nullptr;) {
    auto *next = obs->next;
    delete obs;
    obs = next;
  }
  this->active_observers_ = nullptr;
  for (auto *obs = this->free_observers_; obs != nullptr;) {
    auto *next = obs->next;
    delete obs;
    obs = next;
  }
  this->free_observers_ = nullptr;
  if (this->sock_ >= 0) {
    close(this->sock_);
    this->sock_ = -1;
  }
  return true;
}

void CoapServerNet::dump_config() {
  ESP_LOGCONFIG(TAG, "CoAP Net Server:\n  Listen Port: %d\n  Resources: %" PRIu32, USE_COAP_SERVER_PORT,
                (uint32_t) (this->resources_.size() - 2));
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
}

void CoapServerNet::republish_all() {
  for (auto &res : this->resources_) {
    if (res.observable && res.entity != nullptr)
      this->on_entity_update(res.entity);
  }
}

// ---------------------------------------------------------------------------
// parse_coap
// ---------------------------------------------------------------------------

bool CoapServerNet::parse_coap(const uint8_t *buf, size_t len, CoapPacket *pkt) {
  if (len < 4)
    return false;
  uint8_t ver = (buf[0] >> 6) & 0x03;
  if (ver != 1)
    return false;
  pkt->type = (buf[0] >> 4) & 0x03;
  uint8_t tkl = buf[0] & 0x0F;
  if (tkl > 8)
    return false;
  pkt->code = buf[1];
  pkt->message_id = (uint16_t) ((buf[2] << 8) | buf[3]);
  size_t pos = 4;
  if (pos + tkl > len)
    return false;
  pkt->token_len = tkl;
  memcpy(pkt->token, buf + pos, tkl);
  pos += tkl;

  // Options
  uint16_t opt_num = 0;
  char *path_ptr = pkt->uri_path;
  const char *path_end = pkt->uri_path + sizeof(pkt->uri_path) - 1;
  bool first_seg = true;
  pkt->observe = 3;
  pkt->observe_present = false;
  pkt->oscore_opt_len = 0;
  pkt->payload = nullptr;
  pkt->payload_len = 0;

  while (pos < len) {
    uint8_t b = buf[pos++];
    if (b == 0xFF) {
      pkt->payload = buf + pos;
      pkt->payload_len = (uint16_t) (len - pos);
      break;
    }
    uint16_t delta = (b >> 4) & 0x0F;
    uint16_t opt_len = b & 0x0F;
    if (delta == 13) {
      if (pos >= len)
        return false;
      delta = buf[pos++] + 13;
    } else if (delta == 14) {
      if (pos + 1 >= len)
        return false;
      delta = (uint16_t) (((buf[pos] << 8) | buf[pos + 1]) + 269);
      pos += 2;
    } else if (delta == 15) {
      return false;
    }
    if (opt_len == 13) {
      if (pos >= len)
        return false;
      opt_len = buf[pos++] + 13;
    } else if (opt_len == 14) {
      if (pos + 1 >= len)
        return false;
      opt_len = (uint16_t) (((buf[pos] << 8) | buf[pos + 1]) + 269);
      pos += 2;
    } else if (opt_len == 15) {
      return false;
    }
    opt_num += delta;
    if (pos + opt_len > len)
      return false;
    const uint8_t *oval = buf + pos;
    pos += opt_len;

    switch (opt_num) {
      case 6:  // Observe
        pkt->observe_present = true;
        pkt->observe = (opt_len == 0) ? 0 : oval[opt_len - 1];
        break;
      case 9:  // OSCORE
        if (opt_len <= sizeof(pkt->oscore_opt)) {
          memcpy(pkt->oscore_opt, oval, opt_len);
          pkt->oscore_opt_len = (uint8_t) opt_len;
        }
        break;
      case 11: {  // Uri-Path
        if (!first_seg && path_ptr < path_end)
          *path_ptr++ = '/';
        first_seg = false;
        size_t copy = std::min((size_t) opt_len, (size_t) (path_end - path_ptr));
        memcpy(path_ptr, oval, copy);
        path_ptr += copy;
        break;
      }
      default:
        break;
    }
  }
  *path_ptr = '\0';
  return true;
}

// ---------------------------------------------------------------------------
// process_datagram_ — dispatcher
// ---------------------------------------------------------------------------

void CoapServerNet::process_datagram_(const uint8_t *buf, size_t len, const sockaddr_in6 *peer) {
  CoapPacket pkt{};
  if (!parse_coap(buf, len, &pkt)) {
    ESP_LOGW(TAG, "CoAP Net: malformed packet, dropping");
    return;
  }
  ESP_LOGV(TAG, "CoAP Net: rx type=%u code=0x%02x path=%s", pkt.type, pkt.code, pkt.uri_path);

  if (pkt.type == 2 || pkt.type == 3) {  // ACK or RST — response to a CON notification we sent
    handle_con_response_(pkt, *peer);
    return;
  }

  if (strcmp(pkt.uri_path, ".well-known/core") == 0) {
    handle_well_known_core_(pkt, *peer);
    return;
  }
  if (strcmp(pkt.uri_path, "info") == 0) {
    handle_info_request_(pkt, *peer);
    return;
  }
  if (strcmp(pkt.uri_path, "ping") == 0) {
    handle_ping_request_(pkt, *peer);
    return;
  }

  NetCoapResource *res = find_resource_(pkt.uri_path);
  if (res == nullptr) {
    this->builder_.begin(resp_type(pkt.type), 0x84, pkt.message_id, pkt.token, pkt.token_len);
    send_response(this->builder_.buf, this->builder_.pos, peer);
    return;
  }

#ifdef USE_LOGGER
  if (res->type == ENTITYTYPE_LOG) {
    handle_logs_request_(pkt, *peer);
    return;
  }
#endif
#ifdef USE_BUTTON
  if (res->type == ENTITYTYPE_BUTTON) {
    handle_button_request_(pkt, *peer, res);
    return;
  }
#endif
  handle_entity_request_(pkt, *peer, res);
}

// ---------------------------------------------------------------------------
// send_response
// ---------------------------------------------------------------------------

void CoapServerNet::send_response(const uint8_t *buf, size_t len, const sockaddr_in6 *peer) {
  if (this->sock_ < 0)
    return;
  sendto(this->sock_, buf, len, 0, (const struct sockaddr *) peer, sizeof(*peer));
}

// ---------------------------------------------------------------------------
// handle_well_known_core_
// ---------------------------------------------------------------------------

void CoapServerNet::handle_well_known_core_(const CoapPacket &pkt, const sockaddr_in6 &peer) {
  if (pkt.code != 0x01)
    return;
  this->builder_.begin(resp_type(pkt.type), 0x45, pkt.message_id, pkt.token, pkt.token_len);
  uint8_t cf = 40;  // link-format
  this->builder_.option(12, &cf, 1);
  this->builder_.payload_marker();
  this->builder_.append(this->link_format_buf_.get(), this->link_format_size_);
  send_response(this->builder_.buf, this->builder_.pos, &peer);
}

// ---------------------------------------------------------------------------
// handle_info_request_
// ---------------------------------------------------------------------------

void CoapServerNet::handle_info_request_(const CoapPacket &pkt, const sockaddr_in6 &peer) {
  if (pkt.code != 0x01)
    return;
  uint8_t payload[512];
  size_t payload_len = encode_device_info(payload, sizeof(payload), this);
  if (payload_len == 0) {
    this->builder_.begin(resp_type(pkt.type), 0xA0, pkt.message_id, pkt.token, pkt.token_len);
    send_response(this->builder_.buf, this->builder_.pos, &peer);
    return;
  }
  this->builder_.begin(resp_type(pkt.type), 0x45, pkt.message_id, pkt.token, pkt.token_len);
  uint8_t cf = 60;  // cbor
  this->builder_.option(12, &cf, 1);
  this->builder_.payload_marker();
  this->builder_.append(payload, payload_len);
  send_response(this->builder_.buf, this->builder_.pos, &peer);
}

// ---------------------------------------------------------------------------
// handle_ping_request_
// ---------------------------------------------------------------------------

void CoapServerNet::handle_ping_request_(const CoapPacket &pkt, const sockaddr_in6 &peer) {
  if (pkt.code != 0x01)
    return;

  bool boot_signal = false;
  NetCoapClient *client = find_client_(peer);
  if (client != nullptr) {
    client->last_response_ms = millis();
    client->ping_miss_count = 0;
    if (!client->boot_notified) {
      client->boot_notified = true;
      boot_signal = true;
    }
  } else {
    boot_signal = true;
  }

  uint8_t payload_buf[16];
  size_t plen = this->build_ping_payload(payload_buf, boot_signal);

  this->builder_.begin(resp_type(pkt.type), 0x45, pkt.message_id, pkt.token, pkt.token_len);
  uint8_t cf = 60;
  this->builder_.option(12, &cf, 1);
  this->builder_.payload_marker();
  this->builder_.append(payload_buf, plen);
  send_response(this->builder_.buf, this->builder_.pos, &peer);
}

// ---------------------------------------------------------------------------
// handle_entity_request_
// ---------------------------------------------------------------------------

void CoapServerNet::handle_entity_request_(const CoapPacket &pkt, const sockaddr_in6 &peer, NetCoapResource *resource) {
  touch_client_(peer);

#ifdef USE_COAP_OSCORE
  size_t oscore_plain_len = 0;
  OscoreRequestInfo oscore_req_info{};
  if (!this->oscore_unprotect_request_(pkt, resource, this->oscore_plain_, sizeof(this->oscore_plain_),
                                       &oscore_plain_len, &oscore_req_info)) {
    this->builder_.begin(resp_type(pkt.type), 0x81, pkt.message_id, pkt.token, pkt.token_len);
    send_response(this->builder_.buf, this->builder_.pos, &peer);
    return;
  }
  const bool oscore_protected = (oscore_plain_len > 0);
  uint8_t effective_code = oscore_protected ? this->oscore_plain_[0] : pkt.code;
  const uint8_t *payload_ptr = pkt.payload;
  uint16_t payload_len = pkt.payload_len;
  if (oscore_protected) {
    // extract payload from inner plaintext: skip code + options
    uint8_t inner_pos = 1;
    while (inner_pos < oscore_plain_len) {
      uint8_t dl = this->oscore_plain_[inner_pos++];
      if (dl == 0xFF)
        break;
      uint8_t od = (dl >> 4) & 0x0F;
      uint8_t ol = dl & 0x0F;
      if (od == 13)
        inner_pos++;
      else if (od == 14)
        inner_pos += 2;
      if (ol == 13)
        inner_pos++;
      else if (ol == 14)
        inner_pos += 2;
      else
        inner_pos += ol;
    }
    payload_ptr = this->oscore_plain_ + inner_pos;
    payload_len = (uint16_t) (oscore_plain_len - inner_pos);
  }
#else
  uint8_t effective_code = pkt.code;
  const uint8_t *payload_ptr = pkt.payload;
  uint16_t payload_len = pkt.payload_len;
#endif

  size_t cbor_len = 0;

  if (effective_code == 0x01) {  // GET
    NetCoapObserver *observer = nullptr;
    if (resource->observable && pkt.observe_present) {
      if (pkt.observe == 0) {
        bool is_con = (pkt.type == 0);
        if (this->get_subscription_confirm() != is_con) {
          this->builder_.begin(resp_type(pkt.type), 0x80, pkt.message_id, pkt.token, pkt.token_len);
          send_response(this->builder_.buf, this->builder_.pos, &peer);
          return;
        }
        // Deregister stale observer from same client
        NetCoapObserver *stale = get_observer_(pkt.token, pkt.token_len, peer);
        if (stale != nullptr)
          free_observer_(stale);
        observer = new_observer_(resource, peer, pkt.token, pkt.token_len, is_con);
#ifdef USE_COAP_OSCORE
        if (observer != nullptr && oscore_protected) {
          memcpy(observer->oscore_req_piv, oscore_req_info.piv, oscore_req_info.piv_len);
          observer->oscore_req_piv_len = oscore_req_info.piv_len;
          memcpy(observer->oscore_req_kid, oscore_req_info.kid, oscore_req_info.kid_len);
          observer->oscore_req_kid_len = oscore_req_info.kid_len;
        }
#endif
        if (find_client_(peer) == nullptr)
          new_client_(peer);
      } else if (pkt.observe == 1) {
        NetCoapObserver *obs = get_observer_(pkt.token, pkt.token_len, peer);
        if (obs != nullptr)
          free_observer_(obs);
      }
    }

    cbor_len = cbor_output_(this->cbor_buf_, sizeof(this->cbor_buf_), resource->entity, resource->type);
    if (cbor_len == 0) {
      this->builder_.begin(resp_type(pkt.type), 0xA0, pkt.message_id, pkt.token, pkt.token_len);
      send_response(this->builder_.buf, this->builder_.pos, &peer);
      return;
    }

#ifdef USE_COAP_OSCORE
    if (oscore_protected) {
      size_t inner_len = oscore_build_inner_cbor(0x45, this->cbor_buf_, cbor_len, this->oscore_inner_);
      size_t cipher_len = oscore_protect_response_(this->oscore_inner_, inner_len, oscore_req_info, false,
                                                   this->oscore_cipher_, sizeof(this->oscore_cipher_));
      if (cipher_len == 0) {
        this->builder_.begin(resp_type(pkt.type), 0xA0, pkt.message_id, pkt.token, pkt.token_len);
        send_response(this->builder_.buf, this->builder_.pos, &peer);
        return;
      }
      this->builder_.begin(resp_type(pkt.type), 0x44, pkt.message_id, pkt.token, pkt.token_len);
      if (resource->observable && observer != nullptr && pkt.observe == 0)
        this->builder_.option_uint(6, observer->observe_serial++);
      this->builder_.option_empty(9);  // OSCORE option empty
      this->builder_.payload_marker();
      this->builder_.append(this->oscore_cipher_, cipher_len);
      send_response(this->builder_.buf, this->builder_.pos, &peer);
      return;
    }
#endif
    this->builder_.begin(resp_type(pkt.type), 0x45, pkt.message_id, pkt.token, pkt.token_len);
    if (resource->observable && observer != nullptr && pkt.observe_present && pkt.observe == 0)
      this->builder_.option_uint(6, observer->observe_serial++);
    uint8_t cf = 60;
    this->builder_.option(12, &cf, 1);
    this->builder_.payload_marker();
    this->builder_.append(this->cbor_buf_, cbor_len);
    send_response(this->builder_.buf, this->builder_.pos, &peer);

  } else if (effective_code == 0x02 &&  // POST
             (resource->type == ENTITYTYPE_SWITCH || resource->type == ENTITYTYPE_NUMBER ||
              resource->type == ENTITYTYPE_LOCK || resource->type == ENTITYTYPE_VALVE)) {
    apply_entity_post(resource->entity, resource->type, resource->action, payload_ptr, payload_len);

    cbor_len = cbor_output_(this->cbor_buf_, sizeof(this->cbor_buf_), resource->entity, resource->type);
    if (cbor_len == 0) {
      this->builder_.begin(resp_type(pkt.type), 0xA0, pkt.message_id, pkt.token, pkt.token_len);
      send_response(this->builder_.buf, this->builder_.pos, &peer);
      return;
    }
#ifdef USE_COAP_OSCORE
    if (oscore_protected) {
      size_t inner_len = oscore_build_inner_cbor(0x44, this->cbor_buf_, cbor_len, this->oscore_inner_);
      size_t cipher_len = oscore_protect_response_(this->oscore_inner_, inner_len, oscore_req_info, false,
                                                   this->oscore_cipher_, sizeof(this->oscore_cipher_));
      if (cipher_len == 0) {
        this->builder_.begin(resp_type(pkt.type), 0xA0, pkt.message_id, pkt.token, pkt.token_len);
        send_response(this->builder_.buf, this->builder_.pos, &peer);
        return;
      }
      this->builder_.begin(resp_type(pkt.type), 0x44, pkt.message_id, pkt.token, pkt.token_len);
      this->builder_.option_empty(9);
      this->builder_.payload_marker();
      this->builder_.append(this->oscore_cipher_, cipher_len);
      send_response(this->builder_.buf, this->builder_.pos, &peer);
      return;
    }
#endif
    this->builder_.begin(resp_type(pkt.type), 0x44, pkt.message_id, pkt.token, pkt.token_len);
    uint8_t cf = 60;
    this->builder_.option(12, &cf, 1);
    this->builder_.payload_marker();
    this->builder_.append(this->cbor_buf_, cbor_len);
    send_response(this->builder_.buf, this->builder_.pos, &peer);

  } else {
    this->builder_.begin(resp_type(pkt.type), 0x85, pkt.message_id, pkt.token, pkt.token_len);
    send_response(this->builder_.buf, this->builder_.pos, &peer);
  }
}

// ---------------------------------------------------------------------------
// handle_button_request_
// ---------------------------------------------------------------------------

#ifdef USE_BUTTON
void CoapServerNet::handle_button_request_(const CoapPacket &pkt, const sockaddr_in6 &peer, NetCoapResource *resource) {
  touch_client_(peer);
  if (pkt.code != 0x02) {  // POST
    this->builder_.begin(resp_type(pkt.type), 0x85, pkt.message_id, pkt.token, pkt.token_len);
    send_response(this->builder_.buf, this->builder_.pos, &peer);
    return;
  }
#ifdef USE_COAP_OSCORE
  size_t oscore_plain_len = 0;
  OscoreRequestInfo oscore_req_info{};
  if (!oscore_unprotect_request_(pkt, resource, this->oscore_plain_, sizeof(this->oscore_plain_), &oscore_plain_len,
                                 &oscore_req_info)) {
    this->builder_.begin(resp_type(pkt.type), 0x81, pkt.message_id, pkt.token, pkt.token_len);
    send_response(this->builder_.buf, this->builder_.pos, &peer);
    return;
  }
#endif
  static_cast<button::Button *>(resource->entity)->press();
#ifdef USE_COAP_OSCORE
  if (oscore_plain_len > 0) {
    uint8_t inner[1] = {0x44};
    uint8_t cipher[OSCORE_TAG_LEN + 4];
    size_t cipher_len = oscore_protect_response_(inner, 1, oscore_req_info, false, cipher, sizeof(cipher));
    if (cipher_len > 0) {
      this->builder_.begin(resp_type(pkt.type), 0x44, pkt.message_id, pkt.token, pkt.token_len);
      this->builder_.option_empty(9);
      this->builder_.payload_marker();
      this->builder_.append(cipher, cipher_len);
      send_response(this->builder_.buf, this->builder_.pos, &peer);
    }
    return;
  }
#endif
  this->builder_.begin(resp_type(pkt.type), 0x44, pkt.message_id, pkt.token, pkt.token_len);
  send_response(this->builder_.buf, this->builder_.pos, &peer);
}
#endif

// ---------------------------------------------------------------------------
// handle_con_response_ — ACK/RST from client for a CON notification we sent
// ---------------------------------------------------------------------------

void CoapServerNet::handle_con_response_(const CoapPacket &pkt, const sockaddr_in6 &peer) {
  for (NetCoapObserver **pp = &this->active_observers_; *pp != nullptr; pp = &(*pp)->next) {
    NetCoapObserver *obs = *pp;
    if (!obs->is_con || !obs->con_pending || obs->con_msg_id != pkt.message_id || !addr_equal(obs->peer_addr, peer))
      continue;
    if (pkt.type == 3) {  // RST — client rejects notifications; deregister
      ESP_LOGI(TAG, "CoAP Net: CON notify RST, freeing observer");
      free_observer_(obs);
    } else {  // ACK — notification delivered
      obs->con_pending = false;
    }
    return;
  }
}

// ---------------------------------------------------------------------------
// on_entity_update / notify_observers_
// ---------------------------------------------------------------------------

void CoapServerNet::on_entity_update(EntityBase *entity) {
  if (entity->is_internal())
    return;
  NetCoapResource *resource = nullptr;
  for (size_t i = 0; i < this->resources_.size(); i++) {
    if (this->resources_[i].entity == entity && this->resources_[i].observable) {
      resource = &this->resources_[i];
      break;
    }
  }
  if (resource == nullptr)
    return;
  size_t payload_len = cbor_output_(this->cbor_buf_, sizeof(this->cbor_buf_), resource->entity, resource->type);
  if (payload_len == 0)
    return;
  notify_observers_(resource, this->cbor_buf_, payload_len);
}

void CoapServerNet::notify_observers_(NetCoapResource *resource, const uint8_t *payload, size_t payload_len) {
  for (NetCoapObserver *obs = this->active_observers_; obs != nullptr; obs = obs->next) {
    if (obs->resource != resource)
      continue;
    // Mirror OT: send CON on 1st notification and every 5th after; NON in between.
    bool send_con = obs->is_con && (obs->notify_count == 0 || obs->notify_count == 5);
    if (send_con && obs->con_pending)
      continue;  // waiting for ACK to previous CON before sending another CON
    if (obs->is_con)
      obs->notify_count = (obs->notify_count == 5) ? 1 : (obs->notify_count + 1);

#ifdef USE_COAP_OSCORE
    size_t inner_len = oscore_build_inner_cbor(0x45, payload, payload_len, this->oscore_inner_);
    OscoreRequestInfo aad_info{};
    memcpy(aad_info.piv, obs->oscore_req_piv, obs->oscore_req_piv_len);
    aad_info.piv_len = obs->oscore_req_piv_len;
    memcpy(aad_info.kid, obs->oscore_req_kid, obs->oscore_req_kid_len);
    aad_info.kid_len = obs->oscore_req_kid_len;
    size_t cipher_len = oscore_protect_response_(this->oscore_inner_, inner_len, aad_info, true, this->oscore_cipher_,
                                                 sizeof(this->oscore_cipher_));

    uint8_t oscore_opt[16];
    uint8_t opt_pos = this->oscore_build_notify_option_(oscore_opt);

    if (cipher_len > 0) {
      uint16_t msg_id = this->next_msg_id_++;
      this->builder_.begin(send_con ? 0 : 1, 0x45, msg_id, obs->token, obs->token_len);
      this->builder_.option_uint(6, obs->observe_serial++);
      this->builder_.option(9, oscore_opt, opt_pos);
      this->builder_.payload_marker();
      this->builder_.append(this->oscore_cipher_, cipher_len);
      send_response(this->builder_.buf, this->builder_.pos, &obs->peer_addr);
      if (send_con) {
        obs->con_msg_id = msg_id;
        obs->con_pending = true;
      }
    }
#else
    uint16_t msg_id = this->next_msg_id_++;
    this->builder_.begin(send_con ? 0 : 1, 0x45, msg_id, obs->token, obs->token_len);
    this->builder_.option_uint(6, obs->observe_serial++);
    uint8_t cf = 60;
    this->builder_.option(12, &cf, 1);
    this->builder_.payload_marker();
    this->builder_.append(payload, payload_len);
    send_response(this->builder_.buf, this->builder_.pos, &obs->peer_addr);
    if (send_con) {
      obs->con_msg_id = msg_id;
      obs->con_pending = true;
    }
#endif
  }
}

// ---------------------------------------------------------------------------
// Resource management
// ---------------------------------------------------------------------------

void CoapServerNet::add_net_resource_(EntityType type, EntityBase *entity, bool observable, uint16_t &senml_index) {
  this->resources_.push_back(NetCoapResource());
  NetCoapResource &r = this->resources_[this->resources_.size() - 1];

  strncpy(r.domain, entity_type_domain_name(type), sizeof(r.domain) - 1);

  r.path[0] = 'f';
  r.path[1] = 'p';
  r.path[2] = '/';
  uint8_t pos = 3;
  append_uint16_decimal(r.path, pos, senml_index++);
  r.path[pos++] = '/';
  r.path[pos++] = 'g';
  r.path[pos++] = '/';
  r.path[pos++] = '1';
  r.path[pos] = '\0';

  r.entity = entity;
  r.type = type;
  r.observable = observable;

  ESP_LOGD(TAG, "Add CoAP Net Resource: /%s", r.path);

#ifdef USE_SWITCH
  if (type == ENTITYTYPE_SWITCH) {
    this->resources_.push_back(NetCoapResource());
    NetCoapResource &toggle = this->resources_[this->resources_.size() - 1];
    strcpy(toggle.path, r.path);
    toggle.path[strlen(toggle.path) - 1] = '2';
    strncpy(toggle.domain, r.domain, sizeof(toggle.domain));
    toggle.entity = entity;
    toggle.type = type;
    toggle.action = ACTIONTYPE_TOGGLE;
    toggle.observable = false;
    ESP_LOGD(TAG, "Add CoAP Net Resource: /%s", toggle.path);
  }
#endif
#ifdef USE_VALVE
  if (type == ENTITYTYPE_VALVE) {
    this->resources_.push_back(NetCoapResource());
    NetCoapResource &stop = this->resources_[this->resources_.size() - 1];
    strcpy(stop.path, r.path);
    stop.path[strlen(stop.path) - 1] = '2';
    strncpy(stop.domain, r.domain, sizeof(stop.domain));
    stop.entity = entity;
    stop.type = type;
    stop.action = ACTIONTYPE_STOP;
    stop.observable = false;
    ESP_LOGD(TAG, "Add CoAP Net Resource: /%s", stop.path);
  }
#endif
}

NetCoapResource *CoapServerNet::find_resource_(const char *path) {
  for (size_t i = 0; i < this->resources_.size(); i++) {
    if (strcmp(this->resources_[i].path, path) == 0)
      return &this->resources_[i];
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Client management
// ---------------------------------------------------------------------------

NetCoapClient *CoapServerNet::new_client_(const sockaddr_in6 &peer) {
  NetCoapClient *result = nullptr;
  for (uint8_t i = 0; i < USE_COAP_SERVER_MAX_CLIENTS; i++) {
    auto &c = this->active_clients_[i];
    if (!c.active) {
      c.peer_addr = peer;
      c.active = true;
      c.last_ping_sent_ms = 0;
      c.last_response_ms = millis();
      c.ping_miss_count = 0;
      c.has_non_observer = false;
      c.boot_notified = false;
      c.slot = i;
      this->active_client_count_++;
      for (auto *obs = this->active_observers_; obs != nullptr; obs = obs->next) {
        if (!obs->is_con && addr_equal(obs->peer_addr, peer)) {
          c.has_non_observer = true;
          break;
        }
      }
      result = &c;
      break;
    }
  }
  if (result != nullptr) {
    char addr_str[INET6_ADDRSTRLEN];
    addr_to_str(peer, addr_str, sizeof(addr_str));
    this->client_connected_callback_(std::string(addr_str));
    ping_client_(result);
  }
  return result;
}

NetCoapClient *CoapServerNet::find_client_(const sockaddr_in6 &peer) {
  for (auto &c : this->active_clients_)
    if (c.active && addr_equal(c.peer_addr, peer))
      return &c;
  return nullptr;
}

void CoapServerNet::touch_client_(const sockaddr_in6 &peer) {
  NetCoapClient *c = find_client_(peer);
  if (c != nullptr) {
    c->last_response_ms = millis();
    c->ping_miss_count = 0;
  }
}

void CoapServerNet::free_client_(NetCoapClient *client) {
  cancel_ping_client_(client);
  sockaddr_in6 peer_addr = client->peer_addr;
  client->active = false;
  this->active_client_count_--;

  char addr_str[INET6_ADDRSTRLEN];
  addr_to_str(peer_addr, addr_str, sizeof(addr_str));
  this->client_disconnected_callback_(std::string(addr_str));

  // Free non-CON observers for this client
  NetCoapObserver **pp = &this->active_observers_;
  while (*pp != nullptr) {
    NetCoapObserver *obs = *pp;
    if (!obs->is_con && addr_equal(obs->peer_addr, peer_addr)) {
      *pp = obs->next;
      obs->resource = nullptr;
      obs->next = this->free_observers_;
      this->free_observers_ = obs;
    } else {
      pp = &obs->next;
    }
  }
}

void CoapServerNet::ping_client_(NetCoapClient *client) {
  char key[] = "net_ping_0";
  key[sizeof("net_ping_") - 1] = '0' + client->slot;
  this->set_timeout(key, this->client_ping_interval_ms_, [this, client]() {
    if (!client->active)
      return;
    uint32_t now = millis();
    if (client->last_ping_sent_ms != 0) {
      uint32_t timeout_ms =
          (uint32_t) std::max((this->client_ping_interval_ms_ * this->client_ping_timeout_ratio_), 1000.0f);
      if (now - client->last_response_ms > timeout_ms) {
        if (++client->ping_miss_count >= this->client_ping_retry_) {
          ESP_LOGI(TAG, "CoAP Net: client ping timeout, freeing slot %u", client->slot);
          free_client_(client);
          return;
        }
      }
    }
    if (client->has_non_observer && (now - client->last_response_ms >= this->client_ping_interval_ms_)) {
      // Send a GET /ping to the client to check liveness
      this->builder_.begin(1, 0x01, this->next_msg_id_++, nullptr, 0);  // NON GET
      this->builder_.option(11, (const uint8_t *) "ping", 4);
      client->last_ping_sent_ms = now;
      send_response(this->builder_.buf, this->builder_.pos, &client->peer_addr);
    }
    ping_client_(client);
  });
}

void CoapServerNet::cancel_ping_client_(NetCoapClient *client) {
  char key[] = "net_ping_0";
  key[sizeof("net_ping_") - 1] = '0' + client->slot;
  this->cancel_timeout(key);
}

// ---------------------------------------------------------------------------
// Observer management
// ---------------------------------------------------------------------------

NetCoapObserver *CoapServerNet::get_observer_(const uint8_t *token, uint8_t token_len, const sockaddr_in6 &peer) {
  for (auto *obs = this->active_observers_; obs != nullptr; obs = obs->next) {
    if (obs->token_len == token_len && memcmp(obs->token, token, token_len) == 0 && addr_equal(obs->peer_addr, peer))
      return obs;
  }
  return nullptr;
}

NetCoapObserver *CoapServerNet::new_observer_(NetCoapResource *resource, const sockaddr_in6 &peer, const uint8_t *token,
                                              uint8_t token_len, bool is_con) {
  NetCoapObserver *obs;
  if (this->free_observers_ != nullptr) {
    obs = this->free_observers_;
    this->free_observers_ = obs->next;
    *obs = NetCoapObserver{};
  } else {
    obs = new NetCoapObserver();
  }
  obs->resource = resource;
  obs->peer_addr = peer;
  obs->token_len = token_len;
  memcpy(obs->token, token, token_len);
  obs->observe_serial = 0;
  obs->is_con = is_con;
  obs->next = this->active_observers_;
  this->active_observers_ = obs;
  if (!is_con) {
    NetCoapClient *c = find_client_(peer);
    if (c != nullptr)
      c->has_non_observer = true;
  }
  return obs;
}

void CoapServerNet::free_observer_(NetCoapObserver *observer) {
  NetCoapObserver **pp = &this->active_observers_;
  while (*pp != nullptr && *pp != observer)
    pp = &(*pp)->next;
  if (*pp == observer)
    *pp = observer->next;
  if (!observer->is_con) {
    bool has_non = false;
    for (auto *o = this->active_observers_; o != nullptr; o = o->next) {
      if (!o->is_con && addr_equal(o->peer_addr, observer->peer_addr)) {
        has_non = true;
        break;
      }
    }
    if (!has_non) {
      NetCoapClient *c = find_client_(observer->peer_addr);
      if (c != nullptr)
        c->has_non_observer = false;
    }
  }
  observer->resource = nullptr;
  observer->next = this->free_observers_;
  this->free_observers_ = observer;
}

// ---------------------------------------------------------------------------
// OSCORE (net-specific option extraction)
// ---------------------------------------------------------------------------

#ifdef USE_COAP_OSCORE
bool CoapServerNet::oscore_unprotect_request_(const CoapPacket &pkt, const NetCoapResource *resource,
                                              uint8_t *plaintext, size_t plaintext_buf_len, size_t *plaintext_len,
                                              OscoreRequestInfo *req_info) {
  *plaintext_len = 0;
  if (resource->oscore_exempt)
    return true;
  if (pkt.oscore_opt_len == 0 && pkt.code != 0x05 /* FETCH */) {
    ESP_LOGW(TAG, "OSCORE: request on protected resource /%s without OSCORE option", resource->path);
    return false;
  }
  if (pkt.payload_len == 0 || pkt.payload_len > 256)
    return false;
  return this->oscore_unprotect_core_(pkt.oscore_opt, pkt.oscore_opt_len, pkt.payload, pkt.payload_len, plaintext,
                                      plaintext_buf_len, plaintext_len, req_info);
}
#endif

// ---------------------------------------------------------------------------
// Logger support
// ---------------------------------------------------------------------------

#ifdef USE_LOGGER
void CoapServerNet::on_log(uint8_t level, const char *tag, const char *message, size_t message_len) {
  for (auto *obs = this->active_observers_; obs != nullptr; obs = obs->next) {
    if (obs->resource == this->logs_resource_) {
      this->log_append_entry_(level, tag, message, message_len);
      return;
    }
  }
}

void CoapServerNet::flush_logs_() {
  uint8_t payload[LOG_BUF_SIZE];
  size_t payload_len = this->take_log_payload_(payload);
  if (payload_len > 0)
    notify_observers_(this->logs_resource_, payload, payload_len);
  this->set_timeout("log_flush", 1000, [this]() { this->flush_logs_(); });
}

void CoapServerNet::handle_logs_request_(const CoapPacket &pkt, const sockaddr_in6 &peer) {
  touch_client_(peer);
  if (pkt.code != 0x01)
    return;  // GET only
  if (pkt.observe_present && pkt.observe == 0) {
    bool is_con = (pkt.type == 0);
    if (this->get_subscription_confirm() != is_con) {
      this->builder_.begin(resp_type(pkt.type), 0x80, pkt.message_id, pkt.token, pkt.token_len);
      send_response(this->builder_.buf, this->builder_.pos, &peer);
      return;
    }
    NetCoapObserver *stale = get_observer_(pkt.token, pkt.token_len, peer);
    if (stale != nullptr)
      free_observer_(stale);
    new_observer_(this->logs_resource_, peer, pkt.token, pkt.token_len, is_con);
  } else if (pkt.observe_present && pkt.observe == 1) {
    NetCoapObserver *obs = get_observer_(pkt.token, pkt.token_len, peer);
    if (obs != nullptr)
      free_observer_(obs);
  }
  static const uint8_t kEmpty[] = {0x80};
  this->builder_.begin(resp_type(pkt.type), 0x45, pkt.message_id, pkt.token, pkt.token_len);
  if (pkt.observe_present && pkt.observe == 0)
    this->builder_.option_uint(6, 0);
  uint8_t cf = 60;
  this->builder_.option(12, &cf, 1);
  this->builder_.payload_marker();
  this->builder_.append(kEmpty, sizeof(kEmpty));
  send_response(this->builder_.buf, this->builder_.pos, &peer);
}
#endif

}  // namespace esphome::coap_server

#endif  // NOT USE_OPENTHREAD
