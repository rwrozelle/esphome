// Tests for CoapServerNet: CoAP packet parsing and request dispatch.
// Requests are injected with process_datagram_() and responses captured from send_response().

#include <algorithm>

#include "common.h"

namespace esphome::coap_server::testing {
namespace {

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

TEST(CoapServerNetParse, TooShortReturnsFalse) {
  uint8_t buf[] = {0x51, 0x01};
  CoapServerNet::CoapPacket pkt{};
  EXPECT_FALSE(CoapServerNet::parse_coap(buf, sizeof(buf), &pkt));
}

TEST(CoapServerNetParse, ParseGetPing) {
  auto raw = make_coap_get("ping");
  CoapServerNet::CoapPacket pkt{};
  ASSERT_TRUE(CoapServerNet::parse_coap(raw.data(), raw.size(), &pkt));
  EXPECT_EQ(pkt.type, 1u);
  EXPECT_EQ(pkt.code, 0x01u);
  EXPECT_STREQ(pkt.uri_path, "ping");
  EXPECT_EQ(pkt.token_len, 1u);
  EXPECT_EQ(pkt.token[0], 0xAB);
}

TEST(CoapServerNetParse, ParseWellKnownCore) {
  auto raw = make_coap_get(".well-known/core");
  CoapServerNet::CoapPacket pkt{};
  ASSERT_TRUE(CoapServerNet::parse_coap(raw.data(), raw.size(), &pkt));
  EXPECT_STREQ(pkt.uri_path, ".well-known/core");
}

TEST(CoapServerNetParse, ParseFpPath) {
  auto raw = make_coap_get("fp/1/g/1");
  CoapServerNet::CoapPacket pkt{};
  ASSERT_TRUE(CoapServerNet::parse_coap(raw.data(), raw.size(), &pkt));
  EXPECT_STREQ(pkt.uri_path, "fp/1/g/1");
}

TEST(CoapServerNetParse, ParseObserveRegister) {
  auto raw = make_coap_get("fp/1/g/1", 1, 0);
  CoapServerNet::CoapPacket pkt{};
  ASSERT_TRUE(CoapServerNet::parse_coap(raw.data(), raw.size(), &pkt));
  EXPECT_TRUE(pkt.observe_present);
  EXPECT_EQ(pkt.observe, 0u);
}

TEST(CoapServerNetParse, ParseObserveDeregister) {
  auto raw = make_coap_get("fp/1/g/1", 1, 1);
  CoapServerNet::CoapPacket pkt{};
  ASSERT_TRUE(CoapServerNet::parse_coap(raw.data(), raw.size(), &pkt));
  EXPECT_TRUE(pkt.observe_present);
  EXPECT_EQ(pkt.observe, 1u);
}

TEST(CoapServerNetParse, ParsePostWithPayload) {
  uint8_t payload[] = {0xA1, 0x04, 0xF5};  // {4: true}
  auto raw = make_coap_post("fp/1/g/1", payload, sizeof(payload));
  CoapServerNet::CoapPacket pkt{};
  ASSERT_TRUE(CoapServerNet::parse_coap(raw.data(), raw.size(), &pkt));
  EXPECT_EQ(pkt.code, 0x02u);
  EXPECT_STREQ(pkt.uri_path, "fp/1/g/1");
  ASSERT_EQ(pkt.payload_len, sizeof(payload));
  EXPECT_EQ(memcmp(pkt.payload, payload, sizeof(payload)), 0);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

TEST(CoapServerNetDispatch, PingResponds205) {
  TestableCoapServerNet srv;
  srv.init_resources(2);
  srv.inject(make_coap_get("ping"), make_peer());
  ASSERT_GE(srv.last_sent.size(), 2u);
  EXPECT_EQ(srv.last_sent[1], 0x45u);
}

TEST(CoapServerNetDispatch, InfoResponds205) {
  TestableCoapServerNet srv;
  srv.init_resources(2);
  srv.inject(make_coap_get("info"), make_peer());
  ASSERT_GE(srv.last_sent.size(), 2u);
  EXPECT_EQ(srv.last_sent[1], 0x45u);
}

TEST(CoapServerNetDispatch, WellKnownCoreResponds205) {
  TestableCoapServerNet srv;
  srv.init_resources(2);
  srv.populate_link_format_cache();
  srv.inject(make_coap_get(".well-known/core"), make_peer());
  ASSERT_GE(srv.last_sent.size(), 2u);
  EXPECT_EQ(srv.last_sent[1], 0x45u);
}

TEST(CoapServerNetDispatch, UnknownPathResponds404) {
  TestableCoapServerNet srv;
  srv.init_resources(2);
  srv.inject(make_coap_get("nonexistent"), make_peer());
  ASSERT_GE(srv.last_sent.size(), 2u);
  EXPECT_EQ(srv.last_sent[1], 0x84u);
}

TEST(CoapServerNetDispatch, MirrorsMsgIdAndToken) {
  TestableCoapServerNet srv;
  srv.init_resources(2);
  srv.inject(make_coap_get("ping", 0x1234), make_peer());
  ASSERT_GE(srv.last_sent.size(), 5u);
  EXPECT_EQ(message_id(srv.last_sent), 0x1234u);
  EXPECT_EQ(srv.last_sent[4], 0xABu);
}

TEST(CoapServerNetDispatch, SensorGetResponds205) {
  TestableCoapServerNet srv;
  srv.init_resources(4);
  TestSensor s;
  s.configure("temp");
  s.state = 22.5f;
  uint16_t idx = 1;
  srv.add_resource(ENTITYTYPE_SENSOR, &s, true, idx);

  srv.inject(make_coap_get("fp/1/g/1"), make_peer());
  ASSERT_GE(srv.last_sent.size(), 2u);
  EXPECT_EQ(srv.last_sent[1], 0x45u);
}

TEST(CoapServerNetDispatch, SwitchPostResponds204) {
  TestableCoapServerNet srv;
  srv.init_resources(4);
  TestSwitch sw;
  sw.configure("relay");
  uint16_t idx = 1;
  srv.add_resource(ENTITYTYPE_SWITCH, &sw, true, idx);

  uint8_t cbor[] = {0xA1, 0x04, 0xF5};  // {4: true}
  srv.inject(make_coap_post("fp/1/g/1", cbor, sizeof(cbor)), make_peer());
  ASSERT_GE(srv.last_sent.size(), 2u);
  EXPECT_EQ(srv.last_sent[1], 0x44u);
  EXPECT_TRUE(sw.state);
}

TEST(CoapServerNetDispatch, ButtonPostPresses) {
  TestableCoapServerNet srv;
  srv.init_resources(4);
  TestButton btn;
  btn.configure("restart");
  uint16_t idx = 1;
  srv.add_resource(ENTITYTYPE_BUTTON, &btn, false, idx);

  uint8_t cbor[] = {0xA0};  // {}
  srv.inject(make_coap_post("fp/1/g/1", cbor, sizeof(cbor)), make_peer());
  ASSERT_GE(srv.last_sent.size(), 2u);
  EXPECT_EQ(srv.last_sent[1], 0x44u);
  EXPECT_EQ(btn.press_count, 1);
}

TEST(CoapServerNetDispatch, ButtonGetDoesNotPress) {
  TestableCoapServerNet srv;
  srv.init_resources(4);
  TestButton btn;
  btn.configure("restart");
  uint16_t idx = 1;
  srv.add_resource(ENTITYTYPE_BUTTON, &btn, false, idx);

  srv.inject(make_coap_get("fp/1/g/1"), make_peer());
  ASSERT_GE(srv.last_sent.size(), 2u);
  EXPECT_EQ(srv.last_sent[1], 0x85u);  // 4.05 Method Not Allowed
  EXPECT_EQ(btn.press_count, 0);
}

// ---------------------------------------------------------------------------
// republish_all
// ---------------------------------------------------------------------------

class TrackingNet : public TestableCoapServerNet {
 public:
  std::vector<EntityBase *> updated;

  void push_observable_null_entity() {
    NetCoapResource r{};
    r.observable = true;
    this->resources_.push_back(r);
  }

 protected:
  void on_entity_update(EntityBase *entity) override { this->updated.push_back(entity); }
};

TEST(CoapServerNetRepublish, ObservableEntityIsUpdated) {
  TrackingNet srv;
  srv.init_resources(4);
  TestSensor s;
  uint16_t idx = 1;
  srv.add_resource(ENTITYTYPE_SENSOR, &s, true, idx);

  srv.republish_all();

  ASSERT_EQ(srv.updated.size(), 1u);
  EXPECT_EQ(srv.updated[0], &s);
}

TEST(CoapServerNetRepublish, NonObservableSkipped) {
  TrackingNet srv;
  srv.init_resources(4);
  TestSensor s;
  uint16_t idx = 1;
  srv.add_resource(ENTITYTYPE_SENSOR, &s, false, idx);

  srv.republish_all();

  EXPECT_TRUE(srv.updated.empty());
}

TEST(CoapServerNetRepublish, NullEntitySkipped) {
  TrackingNet srv;
  srv.init_resources(2);
  srv.push_observable_null_entity();

  srv.republish_all();

  EXPECT_TRUE(srv.updated.empty());
}

TEST(CoapServerNetRepublish, MultipleEntitiesAllUpdated) {
  TrackingNet srv;
  srv.init_resources(8);
  TestSensor s1, s2;
  uint16_t idx = 1;
  srv.add_resource(ENTITYTYPE_SENSOR, &s1, true, idx);
  srv.add_resource(ENTITYTYPE_SENSOR, &s2, true, idx);

  srv.republish_all();

  ASSERT_EQ(srv.updated.size(), 2u);
  EXPECT_EQ(srv.updated[0], &s1);
  EXPECT_EQ(srv.updated[1], &s2);
}

// ---------------------------------------------------------------------------
// Client connected / disconnected callbacks
// ---------------------------------------------------------------------------

TEST(CoapServerNetCallbacks, ConnectedCallbackReceivesAddress) {
  TestableCoapServerNet srv;
  srv.init_resources(2);
  std::string got_addr;
  srv.add_on_client_connected_callback([&](const std::string &addr) { got_addr = addr; });

  srv.connect_client(make_peer());

  EXPECT_EQ(got_addr, "::1");
}

TEST(CoapServerNetCallbacks, DisconnectedCallbackFires) {
  TestableCoapServerNet srv;
  srv.init_resources(2);
  std::string disconnected_addr;
  srv.add_on_client_disconnected_callback([&](const std::string &addr) { disconnected_addr = addr; });

  srv.connect_client(make_peer());
  NetCoapClient *c = srv.first_active_client();
  ASSERT_NE(c, nullptr);
  srv.disconnect_client(c);

  EXPECT_EQ(disconnected_addr, "::1");
}

TEST(CoapServerNetCallbacks, ActiveClientCountFollowsConnectAndDisconnect) {
  TestableCoapServerNet srv;
  srv.init_resources(2);

  EXPECT_EQ(srv.get_active_client_count(), 0u);
  srv.connect_client(make_peer());
  EXPECT_EQ(srv.get_active_client_count(), 1u);
  NetCoapClient *c = srv.first_active_client();
  ASSERT_NE(c, nullptr);
  srv.disconnect_client(c);
  EXPECT_EQ(srv.get_active_client_count(), 0u);
}

// ---------------------------------------------------------------------------
// Responses from the client: NON 2.05 to a server ping, ACK/RST to a CON notification
// ---------------------------------------------------------------------------

// NON 2.05 with no Uri-Path: the client's answer to a server-initiated GET /ping
std::vector<uint8_t> make_non_205(uint16_t msg_id = 0x0099) {
  return {0x50, 0x45, static_cast<uint8_t>(msg_id >> 8), static_cast<uint8_t>(msg_id & 0xFF)};
}

TEST(CoapServerNetPingResponse, Non205TouchesClientAndSendsNo404) {
  TestableCoapServerNet srv;
  srv.init_resources(4);
  TestSensor s;
  s.configure("temp");
  s.state = 1.0f;
  uint16_t idx = 1;
  srv.add_resource(ENTITYTYPE_SENSOR, &s, true, idx);

  auto peer = make_peer();
  srv.inject(make_coap_get("fp/1/g/1", 1, 0), peer);
  NetCoapClient *c = srv.first_active_client();
  ASSERT_NE(c, nullptr);
  EXPECT_TRUE(c->has_non_observer);

  // Values touch_client_() must overwrite
  c->last_response_ms = UINT32_MAX;
  c->ping_miss_count = 1;
  srv.last_sent.clear();

  srv.inject(make_non_205(), peer);

  EXPECT_NE(c->last_response_ms, UINT32_MAX);
  EXPECT_EQ(c->ping_miss_count, 0u);
  EXPECT_TRUE(srv.last_sent.empty());
}

// Registers a CON observer for a sensor from peer; returns the observed resource
NetCoapResource *setup_con_observer(TestableCoapServerNet &srv, TestSensor &s, const sockaddr_storage &peer) {
  srv.init_resources(4);
  srv.set_subscription_confirm(true);
  s.configure("temp");
  s.state = 22.5f;
  uint16_t idx = 1;
  srv.add_resource(ENTITYTYPE_SENSOR, &s, true, idx);
  srv.inject(make_coap_get("fp/1/g/1", 10, 0, 0), peer);
  return srv.first_observable_resource();
}

const uint8_t NOTIFY_PAYLOAD[] = {0x81};

TEST(CoapServerNetConObserve, AckClearsPending) {
  TestableCoapServerNet srv;
  TestSensor s;
  NetCoapResource *res = setup_con_observer(srv, s, make_peer());
  ASSERT_NE(res, nullptr);
  NetCoapObserver *obs = srv.first_active_observer();
  ASSERT_NE(obs, nullptr);
  EXPECT_TRUE(obs->is_con);

  srv.trigger_notify(res, NOTIFY_PAYLOAD, sizeof(NOTIFY_PAYLOAD));
  ASSERT_FALSE(srv.last_sent.empty());
  EXPECT_EQ(srv.last_sent[0] & 0x30, 0x00u);  // CON
  EXPECT_TRUE(obs->con_pending);

  srv.inject(make_coap_ack(message_id(srv.last_sent)), make_peer());

  EXPECT_FALSE(obs->con_pending);
  EXPECT_EQ(srv.active_observer_count(), 1u);
}

TEST(CoapServerNetConObserve, RstFreesObserver) {
  TestableCoapServerNet srv;
  TestSensor s;
  NetCoapResource *res = setup_con_observer(srv, s, make_peer());
  ASSERT_NE(res, nullptr);
  ASSERT_EQ(srv.active_observer_count(), 1u);

  srv.trigger_notify(res, NOTIFY_PAYLOAD, sizeof(NOTIFY_PAYLOAD));
  ASSERT_FALSE(srv.last_sent.empty());

  srv.inject(make_coap_rst(message_id(srv.last_sent)), make_peer());

  EXPECT_EQ(srv.active_observer_count(), 0u);
}

TEST(CoapServerNetConObserve, PendingBlocksNextCon) {
  TestableCoapServerNet srv;
  TestSensor s;
  NetCoapResource *res = setup_con_observer(srv, s, make_peer());
  ASSERT_NE(res, nullptr);

  // 1st notification is CON
  srv.trigger_notify(res, NOTIFY_PAYLOAD, sizeof(NOTIFY_PAYLOAD));
  ASSERT_FALSE(srv.last_sent.empty());
  NetCoapObserver *obs = srv.first_active_observer();
  ASSERT_NE(obs, nullptr);
  EXPECT_TRUE(obs->con_pending);
  EXPECT_EQ(obs->notify_count, 1u);

  // 2nd to 5th are NON and are not held back by the pending CON
  for (int i = 0; i < 4; i++) {
    srv.last_sent.clear();
    srv.trigger_notify(res, NOTIFY_PAYLOAD, sizeof(NOTIFY_PAYLOAD));
    ASSERT_FALSE(srv.last_sent.empty()) << "notification " << (i + 2);
    EXPECT_EQ(srv.last_sent[0] & 0x30, 0x10) << "notification " << (i + 2) << " should be NON";
  }
  EXPECT_TRUE(obs->con_pending);
  EXPECT_EQ(obs->notify_count, 5u);

  // 6th would be CON, but the previous CON is still unacknowledged
  srv.last_sent.clear();
  srv.trigger_notify(res, NOTIFY_PAYLOAD, sizeof(NOTIFY_PAYLOAD));
  EXPECT_TRUE(srv.last_sent.empty());
  EXPECT_EQ(obs->notify_count, 5u);
}

TEST(CoapServerNetConObserve, AckFromWrongPeerIgnored) {
  TestableCoapServerNet srv;
  TestSensor s;
  NetCoapResource *res = setup_con_observer(srv, s, make_peer());
  ASSERT_NE(res, nullptr);

  srv.trigger_notify(res, NOTIFY_PAYLOAD, sizeof(NOTIFY_PAYLOAD));
  ASSERT_FALSE(srv.last_sent.empty());
  NetCoapObserver *obs = srv.first_active_observer();
  ASSERT_NE(obs, nullptr);
  EXPECT_TRUE(obs->con_pending);

  srv.inject(make_coap_ack(message_id(srv.last_sent)), make_peer(9999));

  EXPECT_TRUE(obs->con_pending);
}

TEST(CoapServerNetConObserve, DuplicateAckKeepsObserver) {
  TestableCoapServerNet srv;
  TestSensor s;
  NetCoapResource *res = setup_con_observer(srv, s, make_peer());
  ASSERT_NE(res, nullptr);

  srv.trigger_notify(res, NOTIFY_PAYLOAD, sizeof(NOTIFY_PAYLOAD));
  auto ack = make_coap_ack(message_id(srv.last_sent));

  srv.inject(ack, make_peer());
  EXPECT_EQ(srv.active_observer_count(), 1u);
  srv.inject(ack, make_peer());
  EXPECT_EQ(srv.active_observer_count(), 1u);
}

TEST(CoapServerNetConObserve, NonObserverUnaffectedByAck) {
  TestableCoapServerNet srv;
  srv.init_resources(4);
  TestSensor s;
  s.configure("temp");
  s.state = 22.5f;
  uint16_t idx = 1;
  srv.add_resource(ENTITYTYPE_SENSOR, &s, true, idx);

  srv.inject(make_coap_get("fp/1/g/1", 1, 0), make_peer());
  NetCoapObserver *obs = srv.first_active_observer();
  ASSERT_NE(obs, nullptr);
  EXPECT_FALSE(obs->is_con);

  srv.inject(make_coap_ack(0x0001), make_peer());

  EXPECT_EQ(srv.active_observer_count(), 1u);
  EXPECT_FALSE(obs->con_pending);
}

// ---------------------------------------------------------------------------
// Ping boot signal: a client is found by IP alone, so a ping from another source port
// than the observe GET still sees the client and gets -1 only once
// ---------------------------------------------------------------------------

TEST(CoapServerNetPing, BootSignalSentOnceWhenPingPortDiffersFromObservePort) {
  TestableCoapServerNet srv;
  srv.init_resources(4);
  TestSensor s;
  s.configure("temp");
  s.state = 22.5f;
  uint16_t idx = 1;
  srv.add_resource(ENTITYTYPE_SENSOR, &s, true, idx);

  srv.inject(make_coap_get("fp/1/g/1", 1, 0), make_peer(12345));

  srv.inject(make_coap_get("ping", 2), make_peer(9999));
  std::string payload1 = payload_of(srv.last_sent);
  ASSERT_EQ(payload1.size(), 3u);  // {2: -1}
  EXPECT_EQ(static_cast<uint8_t>(payload1[2]), 0x20u);

  srv.inject(make_coap_get("ping", 3), make_peer(9999));
  std::string payload2 = payload_of(srv.last_sent);
  ASSERT_GE(payload2.size(), 3u);
  EXPECT_NE(static_cast<uint8_t>(payload2[2]), 0x20u);  // uptime, not -1
}

// RFC 7252 §4.4: the initial message ID must be random
TEST(CoapServerNetSetup, InitialMsgIdIsRandomized) {
  TestableCoapServerNet a, b;
  // A false failure has a probability of about 1/65536 per run
  EXPECT_NE(a.peek_next_msg_id(), b.peek_next_msg_id());
}

// ---------------------------------------------------------------------------
// IPv4 peers
// ---------------------------------------------------------------------------

TEST(CoapServerNetIPv4, PingResponds205FromIPv4Peer) {
  TestableCoapServerNet srv;
  srv.init_resources(2);
  srv.inject(make_coap_get("ping"), make_peer_ipv4());
  ASSERT_GE(srv.last_sent.size(), 2u);
  EXPECT_EQ(srv.last_sent[1], 0x45u);
}

TEST(CoapServerNetIPv4, ConnectedCallbackReceivesIPv4Address) {
  TestableCoapServerNet srv;
  srv.init_resources(2);
  std::string got_addr;
  srv.add_on_client_connected_callback([&](const std::string &addr) { got_addr = addr; });

  srv.connect_client(make_peer_ipv4("192.168.1.100"));

  EXPECT_EQ(got_addr, "192.168.1.100");
}

TEST(CoapServerNetIPv4, IPv4AndIPv6PeersAreDistinctClients) {
  TestableCoapServerNet srv;
  srv.init_resources(2);

  srv.connect_client(make_peer_ipv4());
  srv.connect_client(make_peer());

  EXPECT_EQ(srv.get_active_client_count(), 2u);
}

TEST(CoapServerNetIPv4, SameIPv4DifferentPortsAreSameClient) {
  TestableCoapServerNet srv;
  srv.init_resources(2);

  srv.connect_client(make_peer_ipv4("10.0.0.1", 5683));
  srv.inject(make_coap_get("ping"), make_peer_ipv4("10.0.0.1", 9999));

  EXPECT_EQ(srv.get_active_client_count(), 1u);
}

TEST(CoapServerNetIPv4, AckFromIPv4WrongPeerIgnored) {
  TestableCoapServerNet srv;
  TestSensor s;
  NetCoapResource *res = setup_con_observer(srv, s, make_peer_ipv4("10.0.0.1"));
  ASSERT_NE(res, nullptr);

  srv.trigger_notify(res, NOTIFY_PAYLOAD, sizeof(NOTIFY_PAYLOAD));
  ASSERT_FALSE(srv.last_sent.empty());
  auto ack = make_coap_ack(message_id(srv.last_sent));
  NetCoapObserver *obs = srv.first_active_observer();
  ASSERT_NE(obs, nullptr);
  EXPECT_TRUE(obs->con_pending);

  srv.inject(ack, make_peer_ipv4("10.0.0.2"));
  EXPECT_TRUE(obs->con_pending);

  srv.inject(ack, make_peer_ipv4("10.0.0.1"));
  EXPECT_FALSE(obs->con_pending);
}

}  // namespace
}  // namespace esphome::coap_server::testing
