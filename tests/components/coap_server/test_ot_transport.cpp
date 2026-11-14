// Tests for the OpenThread transport against the fake OpenThread API in __sdk: .well-known/core over
// OpenThread's block-wise API, republish_all, the button handler, shrink_observers and the client callbacks.

#include "common.h"

#ifdef USE_COAP_SERVER_OPENTHREAD

#include "esphome/components/coap_server/coap_server_ot.h"
#include "esphome/components/openthread/openthread.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "openthread/fake_openthread.h"

namespace esphome::coap_server::testing {
namespace {

class TestableCoapServerOT : public CoapServerOT {
 public:
  TestableCoapServerOT() {
    this->instance_ = openthread::InstanceLock::acquire().get_instance();
    this->resources_.init(4);
    fake_ot::reset();
  }
  // The scheduler outlives this object, and the ping timers capture this
  ~TestableCoapServerOT() {
    for (auto &client : this->active_clients_) {
      if (client.active)
        this->cancel_ping_client_(&client);
    }
    this->teardown();
    if (global_coap_server == this)
      global_coap_server = nullptr;
  }

  void set_link_format(const std::string &content) {
    this->link_format_buf_ = std::make_unique<uint8_t[]>(content.size());
    memcpy(this->link_format_buf_.get(), content.data(), content.size());
    this->link_format_size_ = content.size();
  }
  // The handler's context is the server, as registered in wk_bw_resource_
  void request_well_known_core(otMessage *msg) {
    otMessageInfo info{};
    CoapServerOT::handle_well_known_core(this, msg, &info);
  }
  const BlockwiseSource *wk_source() const { return &this->wk_source_; }

  ehCoapResource *add_resource(EntityType type, EntityBase *entity, bool observable) {
    ehCoapResource r{};
    r.server = this;
    r.entity = entity;
    r.type = type;
    r.observable = observable;
    r.oscore_exempt = true;
    strncpy(r.path, "fp/1/g/1", sizeof(r.path) - 1);
    r.mUriPath = r.path;
    this->resources_.push_back(r);
    ehCoapResource *res = &this->resources_[this->resources_.size() - 1];
    res->mContext = res;
    return res;
  }
  void request_button(ehCoapResource *resource, otMessage *msg) {
    otMessageInfo info{};
    this->handle_button_request(resource, msg, &info);
  }

  void add_free_observer() {
    auto *obs = new ehCoapObserver{};  // NOLINT(cppcoreguidelines-owning-memory) freed by shrink_observers()
    obs->next = this->free_observers_;
    this->free_observers_ = obs;
    this->observer_nodes_++;
  }
  // As add_coap_resource_() does for each observable resource
  void add_observable() { this->add_observable_resource_(); }
  ehCoapObserver *observe(ehCoapResource *resource, uint8_t token) {
    otMessageInfo info{};
    otCoapToken t{};
    t.m8[0] = token;
    t.mLength = 1;
    return this->new_observer_(resource, info, t, OT_COAP_TYPE_NON_CONFIRMABLE);
  }
  void unobserve(ehCoapObserver *obs) {
    LockGuard lock(this->lock_);
    this->free_observer_(obs);
  }
  uint8_t observer_cap() const { return this->observer_cap_; }
  uint8_t observer_nodes() const { return this->observer_nodes_; }
  bool has_free_observers() const { return this->free_observers_ != nullptr; }
  void set_observer_counts(uint8_t active, uint8_t high_water_mark) {
    this->active_count_ = active;
    this->high_water_mark_ = high_water_mark;
  }
  uint8_t high_water_mark() const { return this->high_water_mark_; }

  ehCoapClient *connect_client(const otMessageInfo &info) { return this->new_client_(info); }
  void disconnect_client(ehCoapClient *client) { this->free_client_(client); }

 protected:
#ifdef USE_COAP_OSCORE
  ScopedPrefDir pref_dir_;
#endif
};

otMessage *make_request(otCoapCode code) {
  otMessage *msg = otCoapNewMessage(nullptr, nullptr);
  otCoapMessageInit(msg, OT_COAP_TYPE_NON_CONFIRMABLE, code);
  return msg;
}

std::string make_link_format(int entries) {
  std::string content;
  for (int i = 0; i < entries; i++) {
    if (i > 0)
      content += ',';
    char index[6];
    uint8_t len = 0;
    append_uint16_decimal(index, len, static_cast<uint16_t>(i + 1));
    content += "</fp/";
    content.append(index, len);
    content += ">;rt=\"esphome.sensor\"";
  }
  return content;
}

// ---------------------------------------------------------------------------
// .well-known/core: block-wise when the request has a Block2 option, one message otherwise
// ---------------------------------------------------------------------------

TEST(CoapServerOTWellKnownCore, Block2RequestUsesBlockwiseSend) {
  TestableCoapServerOT srv;
  srv.set_link_format("</fp/1>;rt=\"esphome.sensor\"");
  fake_ot::set_request_block2(6);

  srv.request_well_known_core(make_request(OT_COAP_CODE_GET));

  EXPECT_TRUE(fake_ot::blockwise_called());
  EXPECT_FALSE(fake_ot::inline_send_called());
  EXPECT_EQ(fake_ot::last_response_code(), OT_COAP_CODE_CONTENT);
  EXPECT_EQ(fake_ot::blockwise_ctx(), srv.wk_source());
}

TEST(CoapServerOTWellKnownCore, Block2OptionEchoesRequestSize) {
  TestableCoapServerOT srv;
  srv.set_link_format("</fp/1>;rt=\"esphome.sensor\"");
  fake_ot::set_request_block2(4);

  srv.request_well_known_core(make_request(OT_COAP_CODE_GET));

  ASSERT_TRUE(fake_ot::response_has_block2());
  EXPECT_EQ(fake_ot::response_block2_num(), 0u);  // first block
  EXPECT_TRUE(fake_ot::response_block2_more());   // OpenThread drives the following blocks
  EXPECT_EQ(fake_ot::response_block2_szx(), 4);
}

TEST(CoapServerOTWellKnownCore, BlockwisePayloadIsComplete) {
  TestableCoapServerOT srv;
  const std::string content = make_link_format(40);
  ASSERT_GT(content.size(), 512u);  // more than one block of the fake
  srv.set_link_format(content);
  fake_ot::set_request_block2(6);

  srv.request_well_known_core(make_request(OT_COAP_CODE_GET));

  EXPECT_EQ(fake_ot::last_response(), content);
}

TEST(CoapServerOTWellKnownCore, PlainRequestSendsOneMessage) {
  TestableCoapServerOT srv;
  const std::string content = "</info>;rt=\"esphome.device\"";
  srv.set_link_format(content);

  srv.request_well_known_core(make_request(OT_COAP_CODE_GET));

  EXPECT_TRUE(fake_ot::inline_send_called());
  EXPECT_FALSE(fake_ot::blockwise_called());
  EXPECT_FALSE(fake_ot::response_has_block2());
  EXPECT_EQ(fake_ot::last_response_code(), OT_COAP_CODE_CONTENT);
  EXPECT_EQ(fake_ot::last_response(), content);
}

TEST(CoapServerOTWellKnownCore, PostIsIgnored) {
  TestableCoapServerOT srv;
  srv.set_link_format("</fp/1>;rt=\"esphome.sensor\"");

  srv.request_well_known_core(make_request(OT_COAP_CODE_POST));

  EXPECT_FALSE(fake_ot::blockwise_called());
  EXPECT_FALSE(fake_ot::inline_send_called());
  EXPECT_TRUE(fake_ot::last_response().empty());
}

// ---------------------------------------------------------------------------
// republish_all: one update per observable entity, staggered 50 ms apart on the scheduler
// ---------------------------------------------------------------------------

class TrackingOT : public TestableCoapServerOT {
 public:
  std::vector<EntityBase *> updated;

 protected:
  void on_entity_update(EntityBase *entity) override { this->updated.push_back(entity); }
};

// Past the 0 ms and 50 ms updates
static constexpr uint32_t REPUBLISH_WAIT_MS = 60;

// The updates capture the server, so they must run before it goes away
void run_republish_timers() {
  delay(REPUBLISH_WAIT_MS);
  App.scheduler.call(millis());
}

TEST(CoapServerOTRepublish, ObservableEntitiesUpdatedInOrder) {
  TrackingOT srv;
  TestSwitch sw1, sw2;
  srv.add_resource(ENTITYTYPE_SWITCH, &sw1, true);
  srv.add_resource(ENTITYTYPE_SWITCH, &sw2, true);

  srv.republish_all();
  EXPECT_TRUE(srv.updated.empty());  // nothing runs before the scheduler does
  run_republish_timers();

  ASSERT_EQ(srv.updated.size(), 2u);
  EXPECT_EQ(srv.updated[0], &sw1);
  EXPECT_EQ(srv.updated[1], &sw2);
}

TEST(CoapServerOTRepublish, NonObservableAndNullEntitySkipped) {
  TrackingOT srv;
  TestSwitch sw;
  srv.add_resource(ENTITYTYPE_SWITCH, &sw, false);
  srv.add_resource(ENTITYTYPE_SWITCH, nullptr, true);

  srv.republish_all();
  run_republish_timers();

  EXPECT_TRUE(srv.updated.empty());
}

// ---------------------------------------------------------------------------
// Button
// ---------------------------------------------------------------------------

TEST(CoapServerOTButton, PostPresses) {
  TestableCoapServerOT srv;
  TestButton btn;
  btn.configure("btn");
  ehCoapResource *res = srv.add_resource(ENTITYTYPE_BUTTON, &btn, false);

  srv.request_button(res, make_request(OT_COAP_CODE_POST));

  EXPECT_EQ(btn.press_count, 1);
  EXPECT_EQ(fake_ot::last_response_code(), OT_COAP_CODE_CHANGED);
}

TEST(CoapServerOTButton, GetDoesNotPress) {
  TestableCoapServerOT srv;
  TestButton btn;
  btn.configure("btn");
  ehCoapResource *res = srv.add_resource(ENTITYTYPE_BUTTON, &btn, false);

  srv.request_button(res, make_request(OT_COAP_CODE_GET));

  EXPECT_EQ(btn.press_count, 0);
  EXPECT_EQ(fake_ot::last_response_code(), OT_COAP_CODE_METHOD_NOT_ALLOWED);
}

// ---------------------------------------------------------------------------
// shrink_observers
// ---------------------------------------------------------------------------

TEST(CoapServerOTShrink, FreesPooledObservers) {
  TestableCoapServerOT srv;
  srv.add_free_observer();
  srv.add_free_observer();

  srv.shrink_observers();

  EXPECT_FALSE(srv.has_free_observers());
}

TEST(CoapServerOTShrink, HighWaterMarkResetToActiveCount) {
  TestableCoapServerOT srv;
  srv.set_observer_counts(3, 7);

  srv.shrink_observers();

  EXPECT_EQ(srv.high_water_mark(), 3u);
}

TEST(CoapServerOTShrink, AllowsNewNodesAgain) {
  TestableCoapServerOT srv;
  srv.add_observable();
  for (uint8_t i = 0; i < srv.observer_cap(); i++)
    srv.add_free_observer();

  srv.shrink_observers();

  EXPECT_EQ(srv.observer_nodes(), 0u);
}

// ---------------------------------------------------------------------------
// Observer limit: one node per observable resource per client
// ---------------------------------------------------------------------------

TEST(CoapServerOTObserverLimit, CapFollowsObservableResources) {
  TestableCoapServerOT srv;
  EXPECT_EQ(srv.observer_cap(), 0u);
  srv.add_observable();
  srv.add_observable();
  EXPECT_EQ(srv.observer_cap(), 2 * USE_COAP_SERVER_MAX_CLIENTS);
}

TEST(CoapServerOTObserverLimit, CapClampedTo255) {
  TestableCoapServerOT srv;
  for (int i = 0; i < 300; i++)
    srv.add_observable();
  EXPECT_EQ(srv.observer_cap(), 255u);
}

TEST(CoapServerOTObserverLimit, RefusedAtCap) {
  TestableCoapServerOT srv;
  TestSensor s;
  s.configure("s");
  ehCoapResource *res = srv.add_resource(ENTITYTYPE_SENSOR, &s, true);
  srv.add_observable();

  for (uint8_t i = 0; i < USE_COAP_SERVER_MAX_CLIENTS; i++)
    ASSERT_NE(srv.observe(res, i), nullptr);

  EXPECT_EQ(srv.observe(res, 0xFF), nullptr);
  EXPECT_EQ(srv.observer_nodes(), USE_COAP_SERVER_MAX_CLIENTS);
}

TEST(CoapServerOTObserverLimit, FreedNodeIsReusedAtCap) {
  TestableCoapServerOT srv;
  TestSensor s;
  s.configure("s");
  ehCoapResource *res = srv.add_resource(ENTITYTYPE_SENSOR, &s, true);
  srv.add_observable();
  ehCoapObserver *first = nullptr;
  for (uint8_t i = 0; i < USE_COAP_SERVER_MAX_CLIENTS; i++) {
    ehCoapObserver *obs = srv.observe(res, i);
    ASSERT_NE(obs, nullptr);
    if (first == nullptr)
      first = obs;
  }
  srv.unobserve(first);

  EXPECT_EQ(srv.observe(res, 0xFF), first);
  EXPECT_EQ(srv.observer_nodes(), USE_COAP_SERVER_MAX_CLIENTS);
}

// ---------------------------------------------------------------------------
// Client connected / disconnected callbacks
// ---------------------------------------------------------------------------

TEST(CoapServerOTCallbacks, ConnectAndDisconnectReportTheSameAddress) {
  TestableCoapServerOT srv;
  std::string connected;
  std::string disconnected;
  srv.add_on_client_connected_callback([&](const std::string &addr) { connected = addr; });
  srv.add_on_client_disconnected_callback([&](const std::string &addr) { disconnected = addr; });
  otMessageInfo info{};
  info.mPeerAddr.mFields.m8[0] = 0xfd;
  info.mPeerAddr.mFields.m8[15] = 0x01;

  ehCoapClient *client = srv.connect_client(info);
  ASSERT_NE(client, nullptr);
  EXPECT_EQ(srv.get_active_client_count(), 1u);
  srv.disconnect_client(client);

  EXPECT_FALSE(connected.empty());
  EXPECT_EQ(disconnected, connected);
  EXPECT_EQ(srv.get_active_client_count(), 0u);
}

}  // namespace
}  // namespace esphome::coap_server::testing

#endif  // USE_COAP_SERVER_OPENTHREAD
