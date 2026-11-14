#pragma once

#include "coap_server.h"

#ifdef USE_COAP_SERVER_NETWORK

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cassert>
#include "esphome/components/socket/socket.h"

namespace esphome::coap_server {

// ---------------------------------------------------------------------------
// IP/UDP transport — WiFi, Ethernet, Modem
// ---------------------------------------------------------------------------

using NetCoapResource = CoapResourceBase;

struct NetCoapClient : CoapClientBase {
  sockaddr_storage peer_addr{};
};

struct NetCoapObserver : CoapObserverBase {
  NetCoapObserver *next{nullptr};
  NetCoapResource *resource{nullptr};
  sockaddr_storage peer_addr{};
  uint8_t token[8]{};
  uint8_t token_len{0};
  bool is_con{false};
  uint16_t con_msg_id{0};       // msg_id of the last CON notification sent
  uint16_t con_prev_msg_id{0};  // msg_id of the one it replaced, if it was a resend
  // Retransmission of an unanswered CON notification (RFC 7252 §4.2)
  uint8_t con_retransmits{0};
  uint32_t con_sent_ms{0};
  uint32_t con_timeout_ms{0};
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

  void process_datagram_(const uint8_t *buf, size_t len, const sockaddr_storage *peer);
  virtual void send_response(const uint8_t *buf, size_t len, const sockaddr_storage *peer);

  void handle_well_known_core_(const CoapPacket &pkt, const sockaddr_storage &peer);
  void handle_info_request_(const CoapPacket &pkt, const sockaddr_storage &peer);
  void handle_ping_request_(const CoapPacket &pkt, const sockaddr_storage &peer);
  void handle_entity_request_(const CoapPacket &pkt, const sockaddr_storage &peer, NetCoapResource *resource);
#ifdef USE_BUTTON
  void handle_button_request_(const CoapPacket &pkt, const sockaddr_storage &peer, NetCoapResource *resource);
#endif
#ifdef USE_LOGGER
  void handle_logs_request_(const CoapPacket &pkt, const sockaddr_storage &peer);
  void flush_logs_();
  void on_log(uint8_t level, const char *tag, const char *message, size_t message_len) override;
  NetCoapResource *logs_resource_{nullptr};
#endif

  void handle_con_response_(const CoapPacket &pkt, const sockaddr_storage &peer);
  void notify_observers_(NetCoapResource *resource, const uint8_t *payload, size_t payload_len);
  void send_notification_(NetCoapObserver *obs, const uint8_t *payload, size_t payload_len, bool con, uint32_t now);
  // Resends unanswered CON notifications and frees observers that never answer
  void check_con_timeouts_(uint32_t now);
  void resend_con_(NetCoapObserver *obs, uint32_t now);
  void add_net_resource_(EntityType type, EntityBase *entity, bool observable, uint16_t &senml_index);
  NetCoapResource *find_resource_(const char *path);

  NetCoapClient *new_client_(const sockaddr_storage &peer);
  NetCoapClient *find_client_(const sockaddr_storage &peer);
  void touch_client_(const sockaddr_storage &peer);
  void free_client_(NetCoapClient *client);
  void ping_client_(NetCoapClient *client);
  void cancel_ping_client_(NetCoapClient *client);

  NetCoapObserver *get_observer_(const uint8_t *token, uint8_t token_len, const sockaddr_storage &peer);
  NetCoapObserver *new_observer_(NetCoapResource *resource, const sockaddr_storage &peer, const uint8_t *token,
                                 uint8_t token_len, bool is_con);
  void free_observer_(NetCoapObserver *observer);

#ifdef USE_COAP_OSCORE
  bool oscore_unprotect_request_(const CoapPacket &pkt, const NetCoapResource *resource, uint8_t *plaintext,
                                 size_t plaintext_buf_len, size_t *plaintext_len, OscoreRequestInfo *req_info);
  // Answers a refused request: a protected 4.01 with Echo when unprotect asked for one, a plain 4.01 otherwise
  void send_oscore_refusal_(const CoapPacket &pkt, const sockaddr_storage &peer, const OscoreRequestInfo &req_info);
#endif

  std::unique_ptr<socket::Socket> sock_;
  uint16_t next_msg_id_{static_cast<uint16_t>(random_uint32())};
  FixedVector<NetCoapResource> resources_;
  std::array<NetCoapClient, USE_COAP_SERVER_MAX_CLIENTS> active_clients_{};
  NetCoapObserver *active_observers_{nullptr};
  NetCoapObserver *free_observers_{nullptr};
  uint32_t last_con_check_ms_{0};

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

}  // namespace esphome::coap_server

#endif  // USE_COAP_SERVER_NETWORK
