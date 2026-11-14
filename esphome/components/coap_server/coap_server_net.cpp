#include "coap_server_net.h"
#ifdef USE_COAP_SERVER_NETWORK
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "cbor.h"
#ifdef USE_LOGGER
#include "esphome/components/logger/logger.h"
#endif

namespace esphome::coap_server {

ESPHOME_LOG_TAG(TAG, "coap_server");
static constexpr uint32_t PING_TIMEOUT_ID_BASE = 0;

// RFC 7252 §4.8 defaults, as OpenThread uses: the first wait is ACK_TIMEOUT times a random factor of 1 to 1.5
static constexpr uint32_t COAP_ACK_TIMEOUT_MS = 2000;
static constexpr uint32_t COAP_ACK_RANDOM_SPREAD_MS = 1000;
static constexpr uint8_t COAP_MAX_RETRANSMIT = 4;
// Coarse against the 2 s and longer waits, so loop() does not walk the observers on every pass
static constexpr uint32_t CON_CHECK_INTERVAL_MS = 250;
static const size_t ADDR_STR_LEN = 46;  // large enough for IPv4 and IPv6 (INET6_ADDRSTRLEN)

// Response type: CON(0)→ACK(2), NON(1)→NON(1)
static inline uint8_t resp_type(uint8_t req_type) { return (req_type == 0) ? 2 : 1; }

// ---------------------------------------------------------------------------
// Link-format entry
// ---------------------------------------------------------------------------

static socklen_t sockaddr_len(const sockaddr_storage &addr) {
#if USE_NETWORK_IPV6
  return addr.ss_family == AF_INET6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
#else
  (void) addr;
  // Safe: socket_ip_loop_monitored pins AF_INET at compile time; all peers are IPv4.
  // Would need ss_family check here for a dual-stack socket.
  return sizeof(sockaddr_in);
#endif
}

static bool addr_equal(const sockaddr_storage &a, const sockaddr_storage &b) {
  if (a.ss_family != b.ss_family)
    return false;
#if USE_NETWORK_IPV6
  if (a.ss_family == AF_INET6) {
    const auto &a6 = reinterpret_cast<const sockaddr_in6 &>(a);
    const auto &b6 = reinterpret_cast<const sockaddr_in6 &>(b);
    return memcmp(&a6.sin6_addr, &b6.sin6_addr, sizeof(a6.sin6_addr)) == 0 && a6.sin6_port == b6.sin6_port;
  }
#endif
  const auto &a4 = reinterpret_cast<const sockaddr_in &>(a);
  const auto &b4 = reinterpret_cast<const sockaddr_in &>(b);
  return a4.sin_addr.s_addr == b4.sin_addr.s_addr && a4.sin_port == b4.sin_port;
}

static void addr_to_str(const sockaddr_storage &addr, char *buf, size_t len) {
#if USE_NETWORK_IPV6
  if (addr.ss_family == AF_INET6) {
    inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6 &>(addr).sin6_addr, buf, (socklen_t) len);
    return;
  }
#endif
  inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in &>(addr).sin_addr, buf, (socklen_t) len);
}

// ---------------------------------------------------------------------------
// setup / loop / teardown / dump_config
// ---------------------------------------------------------------------------

void CoapServerNet::setup() {
  CoapServer::setup();
  if (this->is_failed())
    return;

  this->resources_.init(CoapServer::count_resources());

  this->sock_ = socket::socket_ip_loop_monitored(SOCK_DGRAM, IPPROTO_UDP);
  if (!this->sock_) {
    ESP_LOGE(TAG, "CoAP Net: socket() failed");
    this->mark_failed();
    return;
  }
  int yes = 1;
  this->sock_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  sockaddr_storage bind_addr{};
  socklen_t bind_len = socket::set_sockaddr_any((sockaddr *) &bind_addr, sizeof(bind_addr), USE_COAP_SERVER_PORT);
  if (this->sock_->bind((sockaddr *) &bind_addr, bind_len) < 0) {
    ESP_LOGE(TAG, "CoAP Net: bind() failed");
    this->sock_.reset();
    this->mark_failed();
    return;
  }
  this->sock_->setblocking(false);
  ESP_LOGI(TAG, "CoAP Net server started on port %u", USE_COAP_SERVER_PORT);

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
  for (auto *e : App.get_binary_sensors()) {
    if (!e->is_internal()) {
      add_net_resource_(ENTITYTYPE_BINARY_SENSOR, e, true, senml_index);
    }
  }
#endif
#ifdef USE_BUTTON
  for (auto *e : App.get_buttons()) {
    if (!e->is_internal()) {
      add_net_resource_(ENTITYTYPE_BUTTON, e, false, senml_index);
    }
  }
#endif
#ifdef USE_LOCK
  for (auto *e : App.get_locks()) {
    if (!e->is_internal()) {
      add_net_resource_(ENTITYTYPE_LOCK, e, true, senml_index);
    }
  }
#endif
#ifdef USE_NUMBER
  for (auto *e : App.get_numbers()) {
    if (!e->is_internal()) {
      add_net_resource_(ENTITYTYPE_NUMBER, e, true, senml_index);
    }
  }
#endif
#ifdef USE_SENSOR
  for (auto *e : App.get_sensors()) {
    if (!e->is_internal()) {
      add_net_resource_(ENTITYTYPE_SENSOR, e, true, senml_index);
    }
  }
#endif
#ifdef USE_SWITCH
  for (auto *e : App.get_switches()) {
    if (!e->is_internal()) {
      add_net_resource_(ENTITYTYPE_SWITCH, e, true, senml_index);
    }
  }
#endif
#ifdef USE_TEXT_SENSOR
  for (auto *e : App.get_text_sensors()) {
    if (!e->is_internal()) {
      add_net_resource_(ENTITYTYPE_TEXT_SENSOR, e, true, senml_index);
    }
  }
#endif
#ifdef USE_VALVE
  for (auto *e : App.get_valves()) {
    if (!e->is_internal()) {
      add_net_resource_(ENTITYTYPE_VALVE, e, true, senml_index);
    }
  }
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
  // CON observers exist only in CON mode
  if (this->subscription_confirm_) {
    const uint32_t now = App.get_loop_component_start_time();
    if (now - this->last_con_check_ms_ >= CON_CHECK_INTERVAL_MS) {
      this->last_con_check_ms_ = now;
      this->check_con_timeouts_(now);
    }
  }
  if (!this->sock_ || !this->sock_->ready())
    return;
  sockaddr_storage peer{};
  // peerlen is not reset per iteration: safe because socket_ip_loop_monitored pins one
  // address family, so every recvfrom on this socket returns the same address length.
  // A dual-stack socket delivering mixed-family datagrams would require resetting peerlen
  // to sizeof(peer) before each call.
  socklen_t peerlen = sizeof(peer);
  ssize_t n;
  while ((n = this->sock_->recvfrom(this->recv_buf_, sizeof(this->recv_buf_), (sockaddr *) &peer, &peerlen)) > 0)
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
  this->observer_nodes_ = 0;
  if (this->sock_) {
    this->sock_->close();
    this->sock_.reset();
  }
  return true;
}

void CoapServerNet::dump_config() {
  ESP_LOGCONFIG(TAG, "CoAP Net Server:\n  Listen Port: %d\n  Resources: %" PRIu32 "\n  Link-format: %u bytes",
                USE_COAP_SERVER_PORT, (uint32_t) (this->resources_.size() - 1), (unsigned) this->link_format_size_);
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
  pkt->block2_present = false;
  pkt->block2_num = 0;
  pkt->block2_szx = 6;
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
      case 23: {  // Block2 (RFC 7959)
        pkt->block2_present = true;
        uint32_t val = 0;
        for (uint16_t i = 0; i < opt_len && i < 3; i++)
          val = (val << 8) | oval[i];
        pkt->block2_szx = (uint8_t) (val & 0x07);
        pkt->block2_num = val >> 4;  // skip M bit (val >> 3) & 1
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

void CoapServerNet::process_datagram_(const uint8_t *buf, size_t len, const sockaddr_storage *peer) {
  CoapPacket pkt{};
  if (!parse_coap(buf, len, &pkt)) {
    ESP_LOGW(TAG, "CoAP Net: malformed packet, dropping");
    return;
  }
  ESP_LOGV(TAG, "CoAP Net: rx type=%u code=0x%02x path=%s len=%u", pkt.type, pkt.code, pkt.uri_path, (unsigned) len);

  if (pkt.type == 2 || pkt.type == 3) {  // ACK or RST — response to a CON notification we sent
    handle_con_response_(pkt, *peer);
    return;
  }

  if (pkt.type == 1 && (pkt.code & 0xE0) == 0x40) {  // NON 2.xx — response to server-initiated GET /ping
    touch_client_(*peer);
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

void CoapServerNet::send_response(const uint8_t *buf, size_t len, const sockaddr_storage *peer) {
  if (!this->sock_)
    return;
  ESP_LOGV(TAG, "CoAP Net: tx code=0x%02x len=%u", len > 1 ? buf[1] : 0u, (unsigned) len);
  if (this->sock_->sendto(buf, len, 0, (const sockaddr *) peer, sockaddr_len(*peer)) < 0) {
    ESP_LOGW(TAG, "CoAP Net: sendto failed, dropping tx");
  }
}

// ---------------------------------------------------------------------------
// handle_well_known_core_
// ---------------------------------------------------------------------------

void CoapServerNet::handle_well_known_core_(const CoapPacket &pkt, const sockaddr_storage &peer) {
  if (pkt.code != 0x01)
    return;

  // Block size negotiation (RFC 7959 §2.3).
  // Client SZX is capped at 6 (1024 bytes) to stay within builder_.buf budget.
  uint8_t szx = pkt.block2_present ? std::min(pkt.block2_szx, (uint8_t) 6) : 6;
  uint16_t block_size = (uint16_t) (16u << szx);

  // Pre-check before multiplying to avoid uint32_t overflow on large block2_num.
  if ((size_t) pkt.block2_num > this->link_format_size_ / block_size) {
    this->builder_.begin(resp_type(pkt.type), 0x80, pkt.message_id, pkt.token, pkt.token_len);
    send_response(this->builder_.buf, this->builder_.pos, &peer);
    return;
  }
  uint32_t offset = pkt.block2_num * (uint32_t) block_size;

  if (offset >= this->link_format_size_) {
    this->builder_.begin(resp_type(pkt.type), 0x80, pkt.message_id, pkt.token, pkt.token_len);
    send_response(this->builder_.buf, this->builder_.pos, &peer);
    return;
  }

  // Slice directly from the heap-allocated link_format_buf_ — no intermediate copy.
  size_t avail = this->link_format_size_ - offset;
  uint16_t len = (uint16_t) std::min((size_t) block_size, avail);
  bool more = (offset + len) < this->link_format_size_;

  // Block2 option value: (NUM << 4) | (M << 3) | SZX
  uint32_t block2_val = (pkt.block2_num << 4) | (more ? 0x08u : 0x00u) | szx;

  this->builder_.begin(resp_type(pkt.type), 0x45, pkt.message_id, pkt.token, pkt.token_len);
  uint8_t cf = 40;  // link-format
  this->builder_.option(12, &cf, 1);
  ESP_LOGD(TAG, ".well-known/core block %" PRIu32 " offset=%u len=%u more=%d total=%u", (uint32_t) pkt.block2_num,
           (unsigned) offset, (unsigned) len, (int) more, (unsigned) this->link_format_size_);
  this->builder_.option_uint(23, block2_val);
  this->builder_.payload_marker();
  this->builder_.append(this->link_format_buf_.get() + offset, len);
  send_response(this->builder_.buf, this->builder_.pos, &peer);
}

// ---------------------------------------------------------------------------
// handle_info_request_
// ---------------------------------------------------------------------------

void CoapServerNet::handle_info_request_(const CoapPacket &pkt, const sockaddr_storage &peer) {
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

void CoapServerNet::handle_ping_request_(const CoapPacket &pkt, const sockaddr_storage &peer) {
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
  size_t plen = CoapServer::build_ping_payload(payload_buf, boot_signal);

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

void CoapServerNet::handle_entity_request_(const CoapPacket &pkt, const sockaddr_storage &peer,
                                           NetCoapResource *resource) {
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
    size_t inner_pos = 0;
    if (!oscore_inner_payload_offset(this->oscore_plain_, oscore_plain_len, &inner_pos)) {
      this->builder_.begin(resp_type(pkt.type), 0x80, pkt.message_id, pkt.token, pkt.token_len);  // 4.00
      this->send_response(this->builder_.buf, this->builder_.pos, &peer);
      return;
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
          ESP_LOGW(TAG, "CoAP Net: observe type mismatch on /%s (want %s, got %s)", resource->path,
                   this->get_subscription_confirm() ? LOG_STR_LITERAL("CON") : LOG_STR_LITERAL("NON"),
                   is_con ? LOG_STR_LITERAL("CON") : LOG_STR_LITERAL("NON"));
          this->builder_.begin(resp_type(pkt.type), 0x80, pkt.message_id, pkt.token, pkt.token_len);
          send_response(this->builder_.buf, this->builder_.pos, &peer);
          return;
        }
        // Deregister stale observer from same client
        NetCoapObserver *stale = get_observer_(pkt.token, pkt.token_len, peer);
        if (stale != nullptr) {
          ESP_LOGD(TAG, "Replace stale observer from restarted client");
          free_observer_(stale);
        }
        ESP_LOGD(TAG, "Create Observer: /%s", resource->path);
        observer = new_observer_(resource, peer, pkt.token, pkt.token_len, is_con);
#ifdef USE_COAP_OSCORE
        if (observer != nullptr && oscore_protected) {
          memcpy(observer->oscore_req_piv, oscore_req_info.piv, oscore_req_info.piv_len);
          observer->oscore_req_piv_len = oscore_req_info.piv_len;
          memcpy(observer->oscore_req_kid, oscore_req_info.kid, oscore_req_info.kid_len);
          observer->oscore_req_kid_len = oscore_req_info.kid_len;
        }
#endif
        if (observer != nullptr && find_client_(peer) == nullptr)
          new_client_(peer);
      } else if (pkt.observe == 1) {
        NetCoapObserver *obs = get_observer_(pkt.token, pkt.token_len, peer);
        if (obs != nullptr) {
          ESP_LOGD(TAG, "Cancel Observer: /%s", resource->path);
          free_observer_(obs);
        }
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
      size_t inner_len =
          oscore_build_inner_cbor(0x45, this->cbor_buf_, cbor_len, this->oscore_inner_, sizeof(this->oscore_inner_));
      size_t cipher_len = oscore_protect_response_(this->oscore_inner_, inner_len, oscore_req_info, false,
                                                   this->oscore_cipher_, sizeof(this->oscore_cipher_));
      if (cipher_len == 0) {
        this->builder_.begin(resp_type(pkt.type), 0xA0, pkt.message_id, pkt.token, pkt.token_len);
        send_response(this->builder_.buf, this->builder_.pos, &peer);
        return;
      }
      // Outer code 2.04, or 2.05 when the reply carries Observe (RFC 8613 §4.1.3.5)
      const bool with_observe = resource->observable && observer != nullptr && pkt.observe == 0;
      this->builder_.begin(resp_type(pkt.type), with_observe ? 0x45 : 0x44, pkt.message_id, pkt.token, pkt.token_len);
      if (with_observe)
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
      size_t inner_len =
          oscore_build_inner_cbor(0x44, this->cbor_buf_, cbor_len, this->oscore_inner_, sizeof(this->oscore_inner_));
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
void CoapServerNet::handle_button_request_(const CoapPacket &pkt, const sockaddr_storage &peer,
                                           NetCoapResource *resource) {
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
  // A protected request's outer code is always POST; its method is the inner code
  const bool post = oscore_plain_len == 0 || this->oscore_plain_[0] == 0x02;
#else
  const bool post = true;
#endif
  if (post)
    static_cast<button::Button *>(resource->entity)->press();
#ifdef USE_COAP_OSCORE
  if (oscore_plain_len > 0) {
    uint8_t inner[1] = {static_cast<uint8_t>(post ? 0x44 : 0x85)};  // 2.04 Changed or 4.05 Method Not Allowed
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

void CoapServerNet::handle_con_response_(const CoapPacket &pkt, const sockaddr_storage &peer) {
  for (NetCoapObserver **pp = &this->active_observers_; *pp != nullptr; pp = &(*pp)->next) {
    NetCoapObserver *obs = *pp;
    if (!obs->is_con || !obs->con_pending ||
        (obs->con_msg_id != pkt.message_id && obs->con_prev_msg_id != pkt.message_id) ||
        !addr_equal(obs->peer_addr, peer))
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
  for (auto &r : this->resources_) {
    if (r.entity == entity && r.observable) {
      resource = &r;
      break;
    }
  }
  if (resource == nullptr)
    return;
  ESP_LOGV(TAG, "On Update: /%s", resource->path);
  size_t payload_len = cbor_output_(this->cbor_buf_, sizeof(this->cbor_buf_), resource->entity, resource->type);
  if (payload_len == 0)
    return;
  notify_observers_(resource, this->cbor_buf_, payload_len);
}

void CoapServerNet::notify_observers_(NetCoapResource *resource, const uint8_t *payload, size_t payload_len) {
  const uint32_t now = millis();
  for (NetCoapObserver *obs = this->active_observers_; obs != nullptr; obs = obs->next) {
    if (obs->resource != resource)
      continue;
    ESP_LOGV(TAG, "Found Observer: /%s", resource->path);
    // Mirror OT: send CON on 1st notification and every 5th after; NON in between.
    bool send_con = obs->is_con && (obs->notify_count == 0 || obs->notify_count == 5);
    if (send_con && obs->con_pending)
      continue;  // waiting for ACK to previous CON before sending another CON
    if (obs->is_con)
      obs->notify_count = (obs->notify_count == 5) ? 1 : (obs->notify_count + 1);
    if (send_con) {
      obs->con_retransmits = 0;
      obs->con_timeout_ms = COAP_ACK_TIMEOUT_MS + random_uint32() % (COAP_ACK_RANDOM_SPREAD_MS + 1);
    }
    this->send_notification_(obs, payload, payload_len, send_con, now);
  }
}

void CoapServerNet::send_notification_(NetCoapObserver *obs, const uint8_t *payload, size_t payload_len, bool con,
                                       uint32_t now) {
  uint16_t msg_id = this->next_msg_id_++;
#ifdef USE_COAP_OSCORE
#ifdef USE_LOGGER
  // A full log batch must fit, or every one would be dropped (4 bytes of inner header)
  static_assert(LOG_BUF_SIZE + 4 <= sizeof(oscore_inner_), "log batch does not fit oscore_inner_");
  static_assert(LOG_BUF_SIZE + 4 + OSCORE_TAG_LEN <= sizeof(oscore_cipher_), "log batch does not fit oscore_cipher_");
#endif
  size_t inner_len =
      oscore_build_inner_cbor(0x45, payload, payload_len, this->oscore_inner_, sizeof(this->oscore_inner_));
  OscoreRequestInfo aad_info{};
  memcpy(aad_info.piv, obs->oscore_req_piv, obs->oscore_req_piv_len);
  aad_info.piv_len = obs->oscore_req_piv_len;
  memcpy(aad_info.kid, obs->oscore_req_kid, obs->oscore_req_kid_len);
  aad_info.kid_len = obs->oscore_req_kid_len;
  size_t cipher_len = this->oscore_protect_response_(this->oscore_inner_, inner_len, aad_info, true,
                                                     this->oscore_cipher_, sizeof(this->oscore_cipher_));
  if (cipher_len == 0)
    return;
  uint8_t oscore_opt[16];
  uint8_t opt_pos = this->oscore_build_notify_option_(oscore_opt);
  this->builder_.begin(con ? 0 : 1, 0x45, msg_id, obs->token, obs->token_len);
  this->builder_.option_uint(6, obs->observe_serial++);
  this->builder_.option(9, oscore_opt, opt_pos);
  this->builder_.payload_marker();
  this->builder_.append(this->oscore_cipher_, cipher_len);
#else
  this->builder_.begin(con ? 0 : 1, 0x45, msg_id, obs->token, obs->token_len);
  this->builder_.option_uint(6, obs->observe_serial++);
  uint8_t cf = 60;
  this->builder_.option(12, &cf, 1);
  this->builder_.payload_marker();
  this->builder_.append(payload, payload_len);
#endif
  this->send_response(this->builder_.buf, this->builder_.pos, &obs->peer_addr);
  if (con) {
    // A resend replaces the previous message, whose late ACK still confirms the observation
    obs->con_prev_msg_id = obs->con_retransmits > 0 ? obs->con_msg_id : msg_id;
    obs->con_msg_id = msg_id;
    obs->con_pending = true;
    obs->con_sent_ms = now;
  }
}

void CoapServerNet::check_con_timeouts_(uint32_t now) {
  for (NetCoapObserver *obs = this->active_observers_; obs != nullptr;) {
    NetCoapObserver *next = obs->next;  // obs may be freed below
    if (obs->con_pending && now - obs->con_sent_ms >= obs->con_timeout_ms) {
      if (obs->con_retransmits >= COAP_MAX_RETRANSMIT) {
        ESP_LOGI(TAG, "CoAP Net: CON notify not acknowledged, freeing observer");
        this->free_observer_(obs);
      } else {
        obs->con_retransmits++;
        obs->con_timeout_ms *= 2;
        this->resend_con_(obs, now);
      }
    }
    obs = next;
  }
}

// RFC 7641 §4.5.2: the resend carries the current state, as a newer notification may replace one in transit
void CoapServerNet::resend_con_(NetCoapObserver *obs, uint32_t now) {
#ifdef USE_LOGGER
  if (obs->resource == this->logs_resource_) {
    uint8_t payload[LOG_BUF_SIZE];
    this->send_notification_(obs, payload, this->copy_log_payload_(payload), true, now);
    return;
  }
#endif
  size_t len = cbor_output_(this->cbor_buf_, sizeof(this->cbor_buf_), obs->resource->entity, obs->resource->type);
  if (len == 0) {
    obs->con_sent_ms = now;  // nothing to send; wait out this attempt
    return;
  }
  this->send_notification_(obs, this->cbor_buf_, len, true, now);
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
  if (observable)
    this->add_observable_resource_();

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
  for (auto &r : this->resources_) {
    if (strcmp(r.path, path) == 0)
      return &r;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Client management
// ---------------------------------------------------------------------------

NetCoapClient *CoapServerNet::new_client_(const sockaddr_storage &peer) {
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
    char addr_str[ADDR_STR_LEN];
    addr_to_str(peer, addr_str, sizeof(addr_str));
    this->client_connected_callback_(std::string(addr_str));
    ping_client_(result);
  }
  return result;
}

NetCoapClient *CoapServerNet::find_client_(const sockaddr_storage &peer) {
  for (auto &c : this->active_clients_) {
    if (!c.active || c.peer_addr.ss_family != peer.ss_family)
      continue;
#if USE_NETWORK_IPV6
    if (peer.ss_family == AF_INET6) {
      const auto &c6 = reinterpret_cast<const sockaddr_in6 &>(c.peer_addr);
      const auto &p6 = reinterpret_cast<const sockaddr_in6 &>(peer);
      if (memcmp(&c6.sin6_addr, &p6.sin6_addr, sizeof(p6.sin6_addr)) == 0)
        return &c;
      continue;  // must not fall through to IPv4 cast below
    }
#endif
    const auto &c4 = reinterpret_cast<const sockaddr_in &>(c.peer_addr);
    const auto &p4 = reinterpret_cast<const sockaddr_in &>(peer);
    if (c4.sin_addr.s_addr == p4.sin_addr.s_addr)
      return &c;
  }
  return nullptr;
}

void CoapServerNet::touch_client_(const sockaddr_storage &peer) {
  NetCoapClient *c = find_client_(peer);
  if (c != nullptr) {
    c->last_response_ms = millis();
    c->ping_miss_count = 0;
  }
}

void CoapServerNet::free_client_(NetCoapClient *client) {
  cancel_ping_client_(client);
  sockaddr_storage peer_addr = client->peer_addr;
  client->active = false;
  this->active_client_count_--;

  char addr_str[ADDR_STR_LEN];
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
  this->set_timeout(PING_TIMEOUT_ID_BASE + client->slot, this->client_ping_interval_ms_, [this, client]() {
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
  this->cancel_timeout(PING_TIMEOUT_ID_BASE + client->slot);
}

// ---------------------------------------------------------------------------
// Observer management
// ---------------------------------------------------------------------------

NetCoapObserver *CoapServerNet::get_observer_(const uint8_t *token, uint8_t token_len, const sockaddr_storage &peer) {
  for (auto *obs = this->active_observers_; obs != nullptr; obs = obs->next) {
    if (obs->token_len == token_len && memcmp(obs->token, token, token_len) == 0 && addr_equal(obs->peer_addr, peer))
      return obs;
  }
  return nullptr;
}

NetCoapObserver *CoapServerNet::new_observer_(NetCoapResource *resource, const sockaddr_storage &peer,
                                              const uint8_t *token, uint8_t token_len, bool is_con) {
  NetCoapObserver *obs;
  if (this->free_observers_ != nullptr) {
    obs = this->free_observers_;
    this->free_observers_ = obs->next;
    *obs = NetCoapObserver{};
  } else {
    if (this->observer_nodes_ >= this->observer_cap_) {
      ESP_LOGW(TAG, "CoAP Net: observer limit (%u) reached", this->observer_cap_);
      return nullptr;
    }
    obs = new NetCoapObserver();
    this->observer_nodes_++;
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
  if (pkt.oscore_opt_len == 0) {
    ESP_LOGW(TAG, "OSCORE: request on protected resource /%s without OSCORE option", resource->path);
    return false;
  }
  if (pkt.payload_len == 0 || pkt.payload_len > 256) {
    ESP_LOGW(TAG, "OSCORE: request payload_len %u out of range on /%s", pkt.payload_len, resource->path);
    return false;
  }
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

void CoapServerNet::handle_logs_request_(const CoapPacket &pkt, const sockaddr_storage &peer) {
  touch_client_(peer);
  if (pkt.code != 0x01)
    return;  // GET only
  bool registered = false;
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
    registered = new_observer_(this->logs_resource_, peer, pkt.token, pkt.token_len, is_con) != nullptr;
  } else if (pkt.observe_present && pkt.observe == 1) {
    NetCoapObserver *obs = get_observer_(pkt.token, pkt.token_len, peer);
    if (obs != nullptr)
      free_observer_(obs);
  }
  static const uint8_t EMPTY_CBOR_ARRAY[] = {0x80};
  this->builder_.begin(resp_type(pkt.type), 0x45, pkt.message_id, pkt.token, pkt.token_len);
  if (registered)
    this->builder_.option_uint(6, 0);
  uint8_t cf = 60;
  this->builder_.option(12, &cf, 1);
  this->builder_.payload_marker();
  this->builder_.append(EMPTY_CBOR_ARRAY, sizeof(EMPTY_CBOR_ARRAY));
  send_response(this->builder_.buf, this->builder_.pos, &peer);
}
#endif

}  // namespace esphome::coap_server

#endif  // USE_COAP_SERVER_NETWORK
