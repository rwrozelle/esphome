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
#ifdef USE_COAP_OSCORE
    this->oscore_restore_seq_no_();  // as setup() does; a notification needs a saved sequence number
#endif
  }
  // The scheduler outlives this object, and the ping timers capture this
  ~TestableCoapServerOT() {
    App.scheduler.call(millis());  // commands and client announcements deferred to the main loop capture this
    for (auto &client : this->active_clients_) {
      if (client.active)
        this->cancel_ping_client_(&client);
    }
#ifdef USE_LOGGER
    this->cancel_interval(static_cast<uint32_t>(0));  // LOG_FLUSH_ID in coap_server_ot.cpp; the flush captures this
#endif
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
  void request_entity(ehCoapResource *resource, otMessage *msg) {
    otMessageInfo info{};
    this->handle_entity_request(resource, msg, &info, resource->type);
  }
#ifdef USE_LOGGER
  void request_logs(ehCoapResource *resource, otMessage *msg) {
    otMessageInfo info{};
    this->handle_logs_request(resource, msg, &info);
  }
#endif

  void add_free_observer() {
    auto *obs = new ehCoapObserver{};  // NOLINT(cppcoreguidelines-owning-memory) freed by shrink_observers()
    obs->next = this->free_observers_;
    this->free_observers_ = obs;
    this->observer_nodes_++;
  }
  // As add_coap_resource_() does for each observable resource
  void add_observable() { this->add_observable_resource_(); }
  ehCoapObserver *observe(ehCoapResource *resource, uint8_t token, otCoapType type = OT_COAP_TYPE_NON_CONFIRMABLE) {
    otMessageInfo info{};
    otCoapToken t{};
    t.m8[0] = token;
    t.mLength = 1;
    return this->new_observer_(resource, info, t, type);
  }
  void notify(ehCoapResource *resource) {
    static const uint8_t PAYLOAD[] = {0xA1, 0x02, 0x01};
    this->notify_observers_(resource, PAYLOAD, sizeof(PAYLOAD));
  }
  bool is_free(const ehCoapObserver *obs) const {
    for (const ehCoapObserver *cur = this->free_observers_; cur != nullptr; cur = cur->next) {
      if (cur == obs)
        return true;
    }
    return false;
  }
  bool is_releasing(const ehCoapObserver *obs) const {
    for (const ehCoapObserver *cur = this->release_observers_; cur != nullptr; cur = cur->next) {
      if (cur == obs)
        return true;
    }
    return false;
  }
  ehCoapObserver *take_free_observer() {
    ehCoapObserver *obs = this->free_observers_;
    if (obs != nullptr)
      this->free_observers_ = obs->next;
    return obs;
  }
  void finish_con(ehCoapObserver *obs) {
    LockGuard lock(this->lock_);
    this->finish_con_(obs);
  }
  uint8_t active_count() const { return this->active_count_; }
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
#ifdef USE_LOGGER
  void schedule_log_flush() { this->schedule_log_flush_(); }
  void flush_logs() { this->flush_logs_(); }
  uint32_t log_flush_ms() const { return this->log_flush_ms_; }
#endif
  void disconnect_client(ehCoapClient *client) { this->free_client_(client); }

#ifdef USE_COAP_OSCORE
  // Sender ID h'01' and an empty Recipient ID, as in the network transport's OSCORE tests
  void set_oscore_material() {
    static const uint8_t SECRET[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    this->set_oscore_master_secret({SECRET, SECRET + sizeof(SECRET)});
    this->set_oscore_master_salt({});
    this->set_oscore_sender_id({0x01});
    this->set_oscore_recipient_id({});
  }
  bool setup_oscore() {
    this->set_oscore_material();
    // As after a request that returned the Echo value; the Echo exchange has its own tests
    this->oscore_replay_mask_ = 1;
    return this->oscore_derive_keys_();
  }
  void reset_replay_window() { this->oscore_replay_mask_ = 0; }
  // Key derivation clears the master secret
  bool keys_derived() const { return this->oscore_master_secret_.empty(); }
  // Encrypts a request as a client with Sender ID h'' would, or decrypts the reply to it
  bool client_crypt(bool encrypt, const uint8_t *piv, uint8_t piv_len, const uint8_t *in, size_t in_len, uint8_t *out,
                    size_t out_size, size_t *out_len) {
    static const uint8_t NO_BYTES[1] = {};
    uint8_t nonce[OSCORE_IV_LEN];
    oscore_build_nonce(piv, piv_len, NO_BYTES, 0, this->oscore_common_iv_, nonce);
    uint8_t aad[80];
    size_t aad_len = oscore_build_aad(NO_BYTES, 0, piv, piv_len, aad, sizeof(aad));
    if (aad_len == 0)
      return false;
    if (encrypt)
      return oscore_aes_ccm_encrypt(this->oscore_recipient_key_, nonce, aad, aad_len, in, in_len, out, out_size,
                                    out_len);
    return oscore_aes_ccm_decrypt(this->oscore_sender_key_, nonce, aad, aad_len, in, in_len, out, out_size, out_len);
  }
#endif

 protected:
#ifdef USE_COAP_OSCORE
  ScopedPrefDir pref_dir_;
#endif
};

otMessage *make_request(otCoapCode code, otCoapType type = OT_COAP_TYPE_NON_CONFIRMABLE) {
  otMessage *msg = otCoapNewMessage(nullptr, nullptr);
  otCoapMessageInit(msg, type, code);
  return msg;
}

// Runs what the OpenThread handlers deferred to the main loop
void run_main_loop() { App.scheduler.call(millis()); }

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
  EXPECT_EQ(btn.press_count, 0);                // pressed on the main loop, not on the OpenThread task
  EXPECT_FALSE(fake_ot::inline_send_called());  // a NON request gets no ACK

  run_main_loop();

  EXPECT_EQ(btn.press_count, 1);
  EXPECT_EQ(fake_ot::last_request_code(), OT_COAP_CODE_CHANGED);  // separate response
  EXPECT_EQ(fake_ot::last_request_type(), OT_COAP_TYPE_NON_CONFIRMABLE);
}

// A CON request gets an empty ACK at once, then the reply as its own CON message under the request's token
TEST(CoapServerOTButton, ConPostAckedThenAnsweredSeparately) {
  TestableCoapServerOT srv;
  TestButton btn;
  btn.configure("btn");
  ehCoapResource *res = srv.add_resource(ENTITYTYPE_BUTTON, &btn, false);
  otMessage *msg = make_request(OT_COAP_CODE_POST, OT_COAP_TYPE_CONFIRMABLE);
  const uint8_t token[] = {0xBE, 0xEF};
  otCoapMessageSetToken(msg, token, sizeof(token));

  srv.request_button(res, msg);

  EXPECT_EQ(fake_ot::last_response_code(), OT_COAP_CODE_EMPTY);
  EXPECT_EQ(fake_ot::last_request_code(), -1);
  EXPECT_EQ(btn.press_count, 0);

  run_main_loop();

  EXPECT_EQ(btn.press_count, 1);
  EXPECT_EQ(fake_ot::last_request_code(), OT_COAP_CODE_CHANGED);
  EXPECT_EQ(fake_ot::last_request_type(), OT_COAP_TYPE_CONFIRMABLE);
  EXPECT_EQ(fake_ot::last_request_token(), std::string("\xBE\xEF"));
}

TEST(CoapServerOTButton, BusyQueueAnswersServiceUnavailable) {
  TestableCoapServerOT srv;
  TestButton btn;
  btn.configure("btn");
  ehCoapResource *res = srv.add_resource(ENTITYTYPE_BUTTON, &btn, false);

  for (int i = 0; i < 4; i++)
    srv.request_button(res, make_request(OT_COAP_CODE_POST));
  srv.request_button(res, make_request(OT_COAP_CODE_POST));

  EXPECT_EQ(fake_ot::last_response_code(), OT_COAP_CODE_SERVICE_UNAVAILABLE);
  run_main_loop();
  EXPECT_EQ(btn.press_count, 4);
}

// ---------------------------------------------------------------------------
// Entity POST
// ---------------------------------------------------------------------------

TEST(CoapServerOTEntity, SwitchPostRunsOnMainLoopAndRepliesWithState) {
  TestableCoapServerOT srv;
  TestSwitch sw;
  sw.configure("relay");
  ehCoapResource *res = srv.add_resource(ENTITYTYPE_SWITCH, &sw, true);
  otMessage *msg = make_request(OT_COAP_CODE_POST);
  const uint8_t on[] = {0xA1, 0x04, 0xF5};  // {4: true}
  otMessageAppend(msg, on, sizeof(on));

  srv.request_entity(res, msg);
  EXPECT_FALSE(sw.state);

  run_main_loop();

  EXPECT_TRUE(sw.state);
  EXPECT_EQ(fake_ot::last_request_code(), OT_COAP_CODE_CHANGED);
  EXPECT_FALSE(fake_ot::last_request().empty());  // the new state
}

TEST(CoapServerOTEntity, PostPayloadTooLargeIsRefused) {
  TestableCoapServerOT srv;
  TestSwitch sw;
  sw.configure("relay");
  ehCoapResource *res = srv.add_resource(ENTITYTYPE_SWITCH, &sw, true);
  otMessage *msg = make_request(OT_COAP_CODE_POST);
  const uint8_t big[33] = {};
  otMessageAppend(msg, big, sizeof(big));

  srv.request_entity(res, msg);

  EXPECT_EQ(fake_ot::last_response_code(), OT_COAP_CODE_REQUEST_TOO_LARGE);
  run_main_loop();
  EXPECT_EQ(fake_ot::last_request_code(), -1);
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

#ifdef USE_COAP_OSCORE
// Sends a protected button request whose inner message is just inner_code; returns the reply's inner code
int protected_button_request(TestableCoapServerOT &srv, ehCoapResource *res, uint8_t inner_code) {
  const uint8_t piv[] = {0x2C};
  const uint8_t opt[] = {0x09, 0x2C};  // PIV length 1, k flag with an empty KID
  uint8_t ciphertext[64];
  size_t cipher_len = 0;
  if (!srv.client_crypt(true, piv, sizeof(piv), &inner_code, 1, ciphertext, sizeof(ciphertext), &cipher_len))
    return -1;
  fake_ot::set_request_oscore_option(opt, sizeof(opt));
  otMessage *msg = make_request(OT_COAP_CODE_POST);  // the outer code of a protected request
  otMessageAppend(msg, ciphertext, static_cast<uint16_t>(cipher_len));
  srv.request_button(res, msg);
  run_main_loop();
  // A press is answered separately from the main loop; a refused method at once
  std::string reply;
  if (fake_ot::last_request_code() == OT_COAP_CODE_CHANGED) {
    reply = fake_ot::last_request();
  } else if (fake_ot::last_response_code() == OT_COAP_CODE_CHANGED) {
    reply = fake_ot::last_response();
  } else {
    return -1;
  }
  uint8_t plain[64];
  size_t plain_len = 0;
  if (!srv.client_crypt(false, piv, sizeof(piv), reinterpret_cast<const uint8_t *>(reply.data()), reply.size(), plain,
                        sizeof(plain), &plain_len) ||
      plain_len < 1)
    return -1;
  return plain[0];
}

TEST(CoapServerOTButton, ProtectedPostPresses) {
  TestableCoapServerOT srv;
  ASSERT_TRUE(srv.setup_oscore());
  TestButton btn;
  btn.configure("btn");
  ehCoapResource *res = srv.add_resource(ENTITYTYPE_BUTTON, &btn, false);
  res->oscore_exempt = false;

  EXPECT_EQ(protected_button_request(srv, res, OT_COAP_CODE_POST), OT_COAP_CODE_CHANGED);
  EXPECT_EQ(btn.press_count, 1);
}

TEST(CoapServerOTButton, ProtectedInnerGetDoesNotPress) {
  TestableCoapServerOT srv;
  ASSERT_TRUE(srv.setup_oscore());
  TestButton btn;
  btn.configure("btn");
  ehCoapResource *res = srv.add_resource(ENTITYTYPE_BUTTON, &btn, false);
  res->oscore_exempt = false;

  EXPECT_EQ(protected_button_request(srv, res, OT_COAP_CODE_GET), OT_COAP_CODE_METHOD_NOT_ALLOWED);
  EXPECT_EQ(btn.press_count, 0);
}

#ifdef USE_LOGGER
TEST(CoapServerOTLogs, PlainRequestRefusedWithOscore) {
  TestableCoapServerOT srv;
  ASSERT_TRUE(srv.setup_oscore());
  ehCoapResource *res = srv.add_resource(ENTITYTYPE_LOG, nullptr, true);
  res->oscore_exempt = false;

  srv.request_logs(res, make_request(OT_COAP_CODE_GET));

  EXPECT_EQ(fake_ot::last_response_code(), OT_COAP_CODE_UNAUTHORIZED);
}

TEST(CoapServerOTLogs, ProtectedGetAnsweredProtected) {
  TestableCoapServerOT srv;
  ASSERT_TRUE(srv.setup_oscore());
  ehCoapResource *res = srv.add_resource(ENTITYTYPE_LOG, nullptr, true);
  res->oscore_exempt = false;
  const uint8_t piv[] = {0x2D};
  const uint8_t opt[] = {0x09, 0x2D};  // PIV length 1, k flag with an empty KID
  const uint8_t inner[] = {OT_COAP_CODE_GET};
  uint8_t ciphertext[64];
  size_t cipher_len = 0;
  ASSERT_TRUE(
      srv.client_crypt(true, piv, sizeof(piv), inner, sizeof(inner), ciphertext, sizeof(ciphertext), &cipher_len));
  fake_ot::set_request_oscore_option(opt, sizeof(opt));
  otMessage *msg = make_request(OT_COAP_CODE_POST);  // the outer code of a protected request
  otMessageAppend(msg, ciphertext, static_cast<uint16_t>(cipher_len));

  srv.request_logs(res, msg);

  ASSERT_EQ(fake_ot::last_response_code(), OT_COAP_CODE_CHANGED);  // no Observe in this request
  const std::string reply = fake_ot::last_response();
  uint8_t plain[64];
  size_t plain_len = 0;
  ASSERT_TRUE(srv.client_crypt(false, piv, sizeof(piv), reinterpret_cast<const uint8_t *>(reply.data()), reply.size(),
                               plain, sizeof(plain), &plain_len));
  ASSERT_EQ(plain_len, 5u);
  EXPECT_EQ(plain[0], OT_COAP_CODE_CONTENT);
  EXPECT_EQ(plain[4], 0x80);  // CBOR []
}
#endif

// After a reboot the first protected request gets a protected 4.01 with Echo, not a plain 4.01
TEST(CoapServerOTButton, FirstRequestAfterRebootGetsEchoChallenge) {
  TestableCoapServerOT srv;
  ASSERT_TRUE(srv.setup_oscore());
  srv.reset_replay_window();
  TestButton btn;
  btn.configure("btn");
  ehCoapResource *res = srv.add_resource(ENTITYTYPE_BUTTON, &btn, false);
  res->oscore_exempt = false;
  const uint8_t piv[] = {0x2E};
  const uint8_t opt[] = {0x09, 0x2E};
  const uint8_t inner[] = {OT_COAP_CODE_POST};
  uint8_t ciphertext[64];
  size_t cipher_len = 0;
  ASSERT_TRUE(
      srv.client_crypt(true, piv, sizeof(piv), inner, sizeof(inner), ciphertext, sizeof(ciphertext), &cipher_len));
  fake_ot::set_request_oscore_option(opt, sizeof(opt));
  otMessage *msg = make_request(OT_COAP_CODE_POST);
  otMessageAppend(msg, ciphertext, static_cast<uint16_t>(cipher_len));

  srv.request_button(res, msg);

  EXPECT_EQ(fake_ot::last_response_code(), OT_COAP_CODE_CHANGED);  // protected; the 4.01 is inside
  EXPECT_EQ(fake_ot::last_response().size(), 11u + OSCORE_AES_TAG_LEN);
  EXPECT_EQ(btn.press_count, 0);
}
#endif

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

TEST(CoapServerOTObserverLimit, AllObserversOfAResourceAreNotified) {
  // More observers on one resource than a notification batch holds
  TestableCoapServerOT srv;
  TestSensor s;
  s.configure("s");
  ehCoapResource *res = srv.add_resource(ENTITYTYPE_SENSOR, &s, true);
  srv.add_observable();
  srv.add_observable();
  const int observers = USE_COAP_SERVER_MAX_CLIENTS + 1;
  for (int i = 0; i < observers; i++)
    ASSERT_NE(srv.observe(res, static_cast<uint8_t>(i)), nullptr);

  const int before = fake_ot::requests_sent();
  srv.notify(res);

  EXPECT_EQ(fake_ot::requests_sent() - before, observers);
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
// CON notifications: the observer node stays valid while OpenThread holds it as the ACK context
// ---------------------------------------------------------------------------

// Two observable resources, so the observer limit leaves room for a second node
struct ConFixture {
  TestableCoapServerOT srv;
  TestSensor s;
  ehCoapResource *res;
  ehCoapObserver *obs;
  ConFixture() {
    s.configure("s");
    s.state = 1.0f;
    res = srv.add_resource(ENTITYTYPE_SENSOR, &s, true);
    srv.add_observable();
    srv.add_observable();
    obs = srv.observe(res, 1, OT_COAP_TYPE_CONFIRMABLE);
  }
};

TEST(CoapServerOTConNotify, AckClearsPending) {
  ConFixture f;
  f.srv.notify(f.res);
  ASSERT_TRUE(fake_ot::has_pending_request());
  EXPECT_EQ(fake_ot::pending_request_ctx(), f.obs);
  EXPECT_TRUE(f.obs->con_pending);

  fake_ot::complete_pending_request(OT_ERROR_NONE);

  EXPECT_FALSE(f.obs->con_pending);
  EXPECT_EQ(f.srv.active_count(), 1u);
}

TEST(CoapServerOTConNotify, AckTimeoutFreesObserver) {
  ConFixture f;
  f.srv.notify(f.res);

  fake_ot::complete_pending_request(OT_ERROR_RESPONSE_TIMEOUT);

  EXPECT_EQ(f.srv.active_count(), 0u);
  EXPECT_TRUE(f.srv.is_free(f.obs));
}

TEST(CoapServerOTConNotify, NodeFreedWhilePendingIsNotReused) {
  ConFixture f;
  f.srv.notify(f.res);
  f.srv.unobserve(f.obs);

  EXPECT_FALSE(f.srv.is_free(f.obs));
  ehCoapObserver *other = f.srv.observe(f.res, 2, OT_COAP_TYPE_CONFIRMABLE);
  ASSERT_NE(other, nullptr);
  EXPECT_NE(other, f.obs);

  // The late timeout for the first observer must not cancel the second
  fake_ot::complete_pending_request(OT_ERROR_RESPONSE_TIMEOUT);

  EXPECT_TRUE(f.srv.is_free(f.obs));
  EXPECT_EQ(f.srv.active_count(), 1u);
  EXPECT_FALSE(f.srv.is_free(other));
}

TEST(CoapServerOTConNotify, NodeFreedWhilePendingIsReleasedOnAck) {
  ConFixture f;
  f.srv.notify(f.res);
  f.srv.unobserve(f.obs);

  fake_ot::complete_pending_request(OT_ERROR_NONE);

  EXPECT_TRUE(f.srv.is_free(f.obs));
}

TEST(CoapServerOTConNotify, NoMessageBufferClearsPending) {
  ConFixture f;
  fake_ot::fail_next_new_message();
  f.srv.notify(f.res);

  EXPECT_FALSE(f.obs->con_pending);
  EXPECT_FALSE(fake_ot::has_pending_request());

  // Later notifications are still sent
  const int sent = fake_ot::requests_sent();
  f.srv.notify(f.res);
  EXPECT_EQ(fake_ot::requests_sent(), sent + 1);
}

TEST(CoapServerOTConNotify, SendFailureClearsPending) {
  ConFixture f;
  fake_ot::fail_next_send();
  f.srv.notify(f.res);

  EXPECT_FALSE(f.obs->con_pending);
  EXPECT_FALSE(fake_ot::has_pending_request());
}

TEST(CoapServerOTConNotify, NonSendFailureKeepsOtherPendingCon) {
  ConFixture f;
  f.srv.notify(f.res);  // CON, now pending
  ASSERT_TRUE(f.obs->con_pending);

  fake_ot::fail_next_send();
  f.srv.notify(f.res);  // NON, fails

  EXPECT_TRUE(f.obs->con_pending);
}

TEST(CoapServerOTConNotify, FinishConOnNodeMissingFromReleaseList) {
  // Not reachable today: a freed node with con_pending set is always on the release list. Covers the guard that
  // keeps a missing node's next pointer from being linked onto the end of that list.
  ConFixture f;
  f.srv.notify(f.res);
  f.srv.unobserve(f.obs);
  ASSERT_TRUE(f.srv.is_releasing(f.obs));
  f.srv.add_free_observer();
  f.srv.add_free_observer();
  ehCoapObserver *stray = f.srv.take_free_observer();
  ASSERT_NE(stray, nullptr);
  ehCoapObserver *stray_next = stray->next;  // still on the free list
  ASSERT_NE(stray_next, nullptr);
  stray->resource = nullptr;
  stray->con_pending = true;

  f.srv.finish_con(stray);

  EXPECT_FALSE(stray->con_pending);
  EXPECT_TRUE(f.srv.is_free(stray));
  EXPECT_TRUE(f.srv.is_releasing(f.obs));  // the release list is left as it was
  EXPECT_FALSE(f.srv.is_releasing(stray_next));
}

TEST(CoapServerOTConNotify, TeardownWithNodeAwaitingRelease) {
  // Leaves a node on the release list for teardown(); AddressSanitizer reports it if it leaks
  ConFixture f;
  f.srv.notify(f.res);
  f.srv.unobserve(f.obs);
  EXPECT_FALSE(f.srv.is_free(f.obs));
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
  EXPECT_TRUE(connected.empty());  // the automation runs on the main loop
  run_main_loop();
  EXPECT_FALSE(connected.empty());
  srv.disconnect_client(client);

  EXPECT_FALSE(connected.empty());
  EXPECT_EQ(disconnected, connected);
  EXPECT_EQ(srv.get_active_client_count(), 0u);
}

TEST(CoapServerOTCallbacks, ClientFreedBeforeAnnouncedReportsNothing) {
  TestableCoapServerOT srv;
  int events = 0;
  srv.add_on_client_connected_callback([&](const std::string &) { events++; });
  srv.add_on_client_disconnected_callback([&](const std::string &) { events++; });
  otMessageInfo info{};
  info.mPeerAddr.mFields.m8[15] = 0x03;

  ehCoapClient *client = srv.connect_client(info);
  srv.disconnect_client(client);
  run_main_loop();

  EXPECT_EQ(events, 0);
}

// ---------------------------------------------------------------------------
// Client ping interval, run through the real scheduler
// ---------------------------------------------------------------------------

void run_ot_scheduler_for(uint32_t ms) {
  const uint32_t start = millis();
  while (millis() - start < ms) {
    delay(5);
    App.scheduler.call(millis());
  }
}

otMessageInfo client_info() {
  otMessageInfo info{};
  info.mPeerAddr.mFields.m8[0] = 0xfd;
  info.mPeerAddr.mFields.m8[15] = 0x02;
  return info;
}

TEST(CoapServerOTPingInterval, QuietClientIsPinged) {
  TestableCoapServerOT srv;
  srv.set_client_ping_interval(20);
  ehCoapClient *c = srv.connect_client(client_info());
  ASSERT_NE(c, nullptr);
  c->has_non_observer = true;
  c->last_response_ms = millis() - 100;
  const int before = fake_ot::requests_sent();

  run_ot_scheduler_for(60);

  EXPECT_GT(fake_ot::requests_sent(), before);
  EXPECT_NE(c->last_ping_sent_ms, 0u);
  EXPECT_TRUE(c->active);
}

TEST(CoapServerOTPingInterval, ClientThatStopsAnsweringIsFreed) {
  TestableCoapServerOT srv;
  srv.set_client_ping_interval(20);
  ehCoapClient *c = srv.connect_client(client_info());
  ASSERT_NE(c, nullptr);
  c->last_ping_sent_ms = 1;
  c->last_response_ms = millis() - 5000;

  run_ot_scheduler_for(60);

  EXPECT_FALSE(c->active);
  EXPECT_EQ(srv.get_active_client_count(), 0u);
}

#ifdef USE_LOGGER
TEST(CoapServerOTLogFlush, FollowsPollPeriod) {
  TestableCoapServerOT srv;
  srv.schedule_log_flush();
  EXPECT_EQ(srv.log_flush_ms(), 5000u);

  fake_ot::set_poll_period(200);
  srv.flush_logs();
  EXPECT_EQ(srv.log_flush_ms(), 1000u);  // never below 1 s

  fake_ot::set_poll_period(8000);
  srv.flush_logs();
  EXPECT_EQ(srv.log_flush_ms(), 8000u);
}
#endif

TEST(CoapServerOTSetup, CoapStartFailureMarksFailed) {
  TestableCoapServerOT srv;
#ifdef USE_COAP_OSCORE
  srv.set_oscore_material();
#endif
  fake_ot::fail_coap_start();

  srv.setup();

  EXPECT_TRUE(srv.is_failed());
#ifdef USE_COAP_OSCORE
  EXPECT_TRUE(srv.keys_derived());  // so the failure came from starting CoAP
#endif
}

}  // namespace
}  // namespace esphome::coap_server::testing

#endif  // USE_COAP_SERVER_OPENTHREAD
