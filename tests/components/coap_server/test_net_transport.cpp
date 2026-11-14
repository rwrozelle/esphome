// Tests for CoapServerNet: CoAP packet parsing and request dispatch.
// Requests are injected with process_datagram_() and responses captured from send_response().

#include <algorithm>

#include "common.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"

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

// ---------------------------------------------------------------------------
// CON retransmission (RFC 7252 §4.2): resent with the current state, then the observer is freed
// ---------------------------------------------------------------------------

TEST(CoapServerNetConRetransmit, FirstTimeoutIsTwoToThreeSeconds) {
  TestableCoapServerNet srv;
  TestSensor s;
  NetCoapResource *res = setup_con_observer(srv, s, make_peer());
  srv.trigger_notify(res, NOTIFY_PAYLOAD, sizeof(NOTIFY_PAYLOAD));
  NetCoapObserver *obs = srv.first_active_observer();
  ASSERT_NE(obs, nullptr);

  EXPECT_GE(obs->con_timeout_ms, 2000u);
  EXPECT_LE(obs->con_timeout_ms, 3000u);
  EXPECT_EQ(obs->con_retransmits, 0u);
}

TEST(CoapServerNetConRetransmit, ResentWithCurrentStateAfterTimeout) {
  TestableCoapServerNet srv;
  TestSensor s;
  NetCoapResource *res = setup_con_observer(srv, s, make_peer());
  srv.trigger_notify(res, NOTIFY_PAYLOAD, sizeof(NOTIFY_PAYLOAD));
  NetCoapObserver *obs = srv.first_active_observer();
  ASSERT_NE(obs, nullptr);
  const uint16_t first_id = message_id(srv.last_sent);
  const uint32_t sent = obs->con_sent_ms;
  const uint32_t timeout = obs->con_timeout_ms;

  srv.last_sent.clear();
  srv.check_con_timeouts(sent + timeout - 1);
  EXPECT_TRUE(srv.last_sent.empty());

  srv.check_con_timeouts(sent + timeout);
  ASSERT_FALSE(srv.last_sent.empty());
  EXPECT_EQ(srv.last_sent[0] & 0x30, 0x00u);  // CON
  EXPECT_NE(message_id(srv.last_sent), first_id);
  EXPECT_EQ(obs->con_msg_id, message_id(srv.last_sent));
  EXPECT_EQ(obs->con_retransmits, 1u);
  EXPECT_EQ(obs->con_timeout_ms, 2 * timeout);
}

TEST(CoapServerNetConRetransmit, AckToResendStopsRetransmission) {
  TestableCoapServerNet srv;
  TestSensor s;
  NetCoapResource *res = setup_con_observer(srv, s, make_peer());
  srv.trigger_notify(res, NOTIFY_PAYLOAD, sizeof(NOTIFY_PAYLOAD));
  NetCoapObserver *obs = srv.first_active_observer();
  ASSERT_NE(obs, nullptr);
  srv.check_con_timeouts(obs->con_sent_ms + obs->con_timeout_ms);

  srv.inject(make_coap_ack(message_id(srv.last_sent)), make_peer());
  EXPECT_FALSE(obs->con_pending);

  srv.last_sent.clear();
  srv.check_con_timeouts(obs->con_sent_ms + 10 * obs->con_timeout_ms);
  EXPECT_TRUE(srv.last_sent.empty());
  EXPECT_EQ(srv.active_observer_count(), 1u);
}

TEST(CoapServerNetConRetransmit, ObserverFreedAfterLastRetransmitTimesOut) {
  TestableCoapServerNet srv;
  TestSensor s;
  NetCoapResource *res = setup_con_observer(srv, s, make_peer());
  srv.trigger_notify(res, NOTIFY_PAYLOAD, sizeof(NOTIFY_PAYLOAD));
  NetCoapObserver *obs = srv.first_active_observer();
  ASSERT_NE(obs, nullptr);

  int resends = 0;
  while (srv.active_observer_count() == 1 && resends < 10) {
    srv.last_sent.clear();
    srv.check_con_timeouts(obs->con_sent_ms + obs->con_timeout_ms);
    if (!srv.last_sent.empty())
      resends++;
  }

  EXPECT_EQ(resends, 4);
  EXPECT_EQ(srv.active_observer_count(), 0u);
}

TEST(CoapServerNetConRetransmit, LoopChecksTimeouts) {
  TestableCoapServerNet srv;
  TestSensor s;
  NetCoapResource *res = setup_con_observer(srv, s, make_peer());
  srv.trigger_notify(res, NOTIFY_PAYLOAD, sizeof(NOTIFY_PAYLOAD));
  NetCoapObserver *obs = srv.first_active_observer();
  ASSERT_NE(obs, nullptr);
  const uint32_t now = App.get_loop_component_start_time();
  obs->con_sent_ms = now - obs->con_timeout_ms;
  srv.set_last_con_check(now - 250);

  srv.loop();

  EXPECT_EQ(obs->con_retransmits, 1u);
}

TEST(CoapServerNetConRetransmit, LoopSkipsCheckWithin250ms) {
  TestableCoapServerNet srv;
  TestSensor s;
  NetCoapResource *res = setup_con_observer(srv, s, make_peer());
  srv.trigger_notify(res, NOTIFY_PAYLOAD, sizeof(NOTIFY_PAYLOAD));
  NetCoapObserver *obs = srv.first_active_observer();
  ASSERT_NE(obs, nullptr);
  const uint32_t now = App.get_loop_component_start_time();
  obs->con_sent_ms = now - obs->con_timeout_ms;
  srv.set_last_con_check(now - 249);

  srv.loop();

  EXPECT_EQ(obs->con_retransmits, 0u);
}

TEST(CoapServerNetConRetransmit, LateAckForReplacedMessageConfirms) {
  TestableCoapServerNet srv;
  TestSensor s;
  NetCoapResource *res = setup_con_observer(srv, s, make_peer());
  srv.trigger_notify(res, NOTIFY_PAYLOAD, sizeof(NOTIFY_PAYLOAD));
  NetCoapObserver *obs = srv.first_active_observer();
  ASSERT_NE(obs, nullptr);
  const uint16_t first_id = message_id(srv.last_sent);
  srv.check_con_timeouts(obs->con_sent_ms + obs->con_timeout_ms);
  ASSERT_NE(obs->con_msg_id, first_id);

  srv.inject(make_coap_ack(first_id), make_peer());

  EXPECT_FALSE(obs->con_pending);
}

TEST(CoapServerNetConRetransmit, AckOlderThanReplacedMessageIgnored) {
  TestableCoapServerNet srv;
  TestSensor s;
  NetCoapResource *res = setup_con_observer(srv, s, make_peer());
  srv.trigger_notify(res, NOTIFY_PAYLOAD, sizeof(NOTIFY_PAYLOAD));
  NetCoapObserver *obs = srv.first_active_observer();
  ASSERT_NE(obs, nullptr);
  const uint16_t first_id = message_id(srv.last_sent);
  srv.check_con_timeouts(obs->con_sent_ms + obs->con_timeout_ms);
  srv.check_con_timeouts(obs->con_sent_ms + obs->con_timeout_ms);
  ASSERT_EQ(obs->con_retransmits, 2u);

  srv.inject(make_coap_ack(first_id), make_peer());

  EXPECT_TRUE(obs->con_pending);
}

TEST(CoapServerNetConRetransmit, ResendSkippedWhenStateCannotBeEncoded) {
  TestableCoapServerNet srv;
  srv.init_resources(2);
  srv.set_subscription_confirm(true);
  TestTextSensor ts;
  ts.configure("ts");
  ts.state = "short";
  uint16_t idx = 1;
  srv.add_resource(ENTITYTYPE_TEXT_SENSOR, &ts, true, idx);
  srv.inject(make_coap_get("fp/1/g/1", 10, 0, 0), make_peer());
  srv.trigger_notify(srv.first_observable_resource(), NOTIFY_PAYLOAD, sizeof(NOTIFY_PAYLOAD));
  NetCoapObserver *obs = srv.first_active_observer();
  ASSERT_NE(obs, nullptr);
  ASSERT_TRUE(obs->con_pending);
  ts.state = std::string(400, 'x');  // larger than the CBOR buffer
  const uint32_t due = obs->con_sent_ms + obs->con_timeout_ms;
  srv.last_sent.clear();

  srv.check_con_timeouts(due);

  EXPECT_TRUE(srv.last_sent.empty());
  EXPECT_TRUE(obs->con_pending);
  EXPECT_EQ(obs->con_sent_ms, due);  // waits out this attempt
  EXPECT_EQ(obs->con_retransmits, 1u);
}

#ifdef USE_COAP_OSCORE
TEST(CoapServerNetConRetransmit, NotificationNotSentWhenEncryptionFails) {
  TestableCoapServerNet srv;
  TestSensor s;
  NetCoapResource *res = setup_con_observer(srv, s, make_peer());
  NetCoapObserver *obs = srv.first_active_observer();
  ASSERT_NE(obs, nullptr);
  srv.set_sender_seq(UINT32_MAX);  // exhausted, so encryption fails
  srv.last_sent.clear();

  srv.trigger_notify(res, NOTIFY_PAYLOAD, sizeof(NOTIFY_PAYLOAD));

  EXPECT_TRUE(srv.last_sent.empty());
  EXPECT_FALSE(obs->con_pending);
  EXPECT_TRUE(srv.status_has_error());
  EXPECT_FALSE(srv.is_failed());  // after setup the component reports an error instead of failing
}
#endif

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

// ---------------------------------------------------------------------------
// Observer limit: one node per observable resource per client
// ---------------------------------------------------------------------------

// The first option after the 1-byte token is Observe (delta 6) only when the observer was registered
bool response_has_observe(const std::vector<uint8_t> &pkt) { return pkt.size() > 5 && (pkt[5] >> 4) == 6; }

TEST(CoapServerNetObserverLimit, CapCountsOnlyObservableResources) {
  TestableCoapServerNet srv;
  srv.init_resources(4);
  TestSensor s;
  s.configure("s");
  TestSwitch sw;
  sw.configure("sw");
  uint16_t idx = 1;
  srv.add_resource(ENTITYTYPE_SENSOR, &s, true, idx);
  srv.add_resource(ENTITYTYPE_SWITCH, &sw, true, idx);  // its toggle resource is not observable

  EXPECT_EQ(srv.observer_cap(), 2 * USE_COAP_SERVER_MAX_CLIENTS);
}

TEST(CoapServerNetObserverLimit, RefusedAtCapAnsweredWithoutObserve) {
  TestableCoapServerNet srv;
  srv.init_resources(2);
  TestSensor s;
  s.configure("s");
  s.state = 1.0f;
  uint16_t idx = 1;
  srv.add_resource(ENTITYTYPE_SENSOR, &s, true, idx);

  for (uint16_t port = 1; port <= USE_COAP_SERVER_MAX_CLIENTS; port++) {
    srv.inject(make_coap_get("fp/1/g/1", port, 0), make_peer(port));
    ASSERT_TRUE(response_has_observe(srv.last_sent));
  }
  srv.inject(make_coap_get("fp/1/g/1", 100, 0), make_peer(100));

  EXPECT_EQ(srv.last_sent[1], 0x45u);  // still answered with the state
  EXPECT_FALSE(response_has_observe(srv.last_sent));
  EXPECT_EQ(srv.active_observer_count(), USE_COAP_SERVER_MAX_CLIENTS);
  EXPECT_EQ(srv.observer_nodes(), USE_COAP_SERVER_MAX_CLIENTS);
}

TEST(CoapServerNetObserverLimit, FreedNodeIsReusedAtCap) {
  TestableCoapServerNet srv;
  srv.init_resources(2);
  TestSensor s;
  s.configure("s");
  s.state = 1.0f;
  uint16_t idx = 1;
  srv.add_resource(ENTITYTYPE_SENSOR, &s, true, idx);
  for (uint16_t port = 1; port <= USE_COAP_SERVER_MAX_CLIENTS; port++)
    srv.inject(make_coap_get("fp/1/g/1", port, 0), make_peer(port));

  srv.inject(make_coap_get("fp/1/g/1", 50, 1), make_peer(1));  // deregister
  srv.inject(make_coap_get("fp/1/g/1", 51, 0), make_peer(100));

  EXPECT_TRUE(response_has_observe(srv.last_sent));
  EXPECT_EQ(srv.active_observer_count(), USE_COAP_SERVER_MAX_CLIENTS);
  EXPECT_EQ(srv.observer_nodes(), USE_COAP_SERVER_MAX_CLIENTS);
}

TEST(CoapServerNetObserverLimit, RefusedObserverDoesNotTakeAClientSlot) {
  TestableCoapServerNet srv;
  srv.init_resources(2);
  TestSensor s;
  s.configure("s");
  s.state = 1.0f;
  uint16_t idx = 1;
  srv.add_resource(ENTITYTYPE_SENSOR, &s, true, idx);
  for (uint16_t port = 1; port <= USE_COAP_SERVER_MAX_CLIENTS; port++)
    srv.inject(make_coap_get("fp/1/g/1", port, 0), make_peer(port));
  const uint8_t clients = srv.get_active_client_count();

  sockaddr_storage other = make_peer_ipv4("127.0.0.2", 100);
  srv.inject(make_coap_get("fp/1/g/1", 100, 0), other);

  EXPECT_EQ(srv.get_active_client_count(), clients);
}

// ---------------------------------------------------------------------------
// Client ping interval, run through the real scheduler
// ---------------------------------------------------------------------------

// Lets the scheduler run whatever falls due during ms milliseconds
void run_scheduler_for(uint32_t ms) {
  const uint32_t start = millis();
  while (millis() - start < ms) {
    delay(5);
    App.scheduler.call(millis());
  }
}

TEST(CoapServerNetPingInterval, QuietClientIsPingedRepeatedly) {
  TestableCoapServerNet srv;
  srv.set_client_ping_interval(20);
  NetCoapClient *c = srv.connect_client(make_peer());
  ASSERT_NE(c, nullptr);
  c->has_non_observer = true;
  c->last_response_ms = millis() - 100;  // quiet for longer than the interval, within the 1 s timeout
  srv.last_sent.clear();

  run_scheduler_for(60);
  ASSERT_FALSE(srv.last_sent.empty());
  EXPECT_EQ(srv.last_sent[1], 0x01u);  // GET
  EXPECT_NE(std::string(srv.last_sent.begin(), srv.last_sent.end()).find("ping"), std::string::npos);
  const uint16_t first_id = message_id(srv.last_sent);

  run_scheduler_for(60);
  EXPECT_NE(message_id(srv.last_sent), first_id);  // the interval keeps running
  EXPECT_TRUE(c->active);
}

TEST(CoapServerNetPingInterval, ClientThatStopsAnsweringIsFreed) {
  TestableCoapServerNet srv;
  srv.set_client_ping_interval(20);
  NetCoapClient *c = srv.connect_client(make_peer());
  ASSERT_NE(c, nullptr);
  c->last_ping_sent_ms = 1;
  c->last_response_ms = millis() - 5000;  // past the 1 s minimum timeout

  run_scheduler_for(60);

  EXPECT_FALSE(c->active);
  EXPECT_EQ(srv.get_active_client_count(), 0u);
}

}  // namespace
}  // namespace esphome::coap_server::testing
