// Tests for entity commands: CBOR POST payloads applied through apply_entity_post().
// Resource paths: fp/1/g/1 is the entity, fp/1/g/2 its action (toggle for a switch, stop for a valve).

#include "common.h"

namespace esphome::coap_server::testing {
namespace {

const uint8_t TRUE_MAP[] = {0xA1, 0x04, 0xF5};   // {4: true}
const uint8_t FALSE_MAP[] = {0xA1, 0x04, 0xF4};  // {4: false}

class CommandServer : public TestableCoapServerNet {
 public:
  explicit CommandServer(EntityType type, EntityBase *entity) {
    this->init_resources(4);
    uint16_t idx = 1;
    this->add_resource(type, entity, true, idx);
  }
  void post(const char *path, const uint8_t *payload, size_t len) {
    this->inject(make_coap_post(path, payload, len), make_peer());
  }
  uint8_t response_code() const { return this->last_sent.size() >= 2 ? this->last_sent[1] : 0; }
};

// ---------------------------------------------------------------------------
// Switch
// ---------------------------------------------------------------------------

TEST(EntityCommands, SwitchTurnOn) {
  TestSwitch sw;
  sw.configure("sw");
  CommandServer srv(ENTITYTYPE_SWITCH, &sw);

  srv.post("fp/1/g/1", TRUE_MAP, sizeof(TRUE_MAP));

  EXPECT_TRUE(sw.state);
  EXPECT_EQ(srv.response_code(), 0x44u);
}

TEST(EntityCommands, SwitchTurnOff) {
  TestSwitch sw;
  sw.configure("sw");
  sw.state = true;
  CommandServer srv(ENTITYTYPE_SWITCH, &sw);

  srv.post("fp/1/g/1", FALSE_MAP, sizeof(FALSE_MAP));

  EXPECT_FALSE(sw.state);
}

TEST(EntityCommands, SwitchToggle) {
  TestSwitch sw;
  sw.configure("sw");
  CommandServer srv(ENTITYTYPE_SWITCH, &sw);

  srv.post("fp/1/g/2", TRUE_MAP, sizeof(TRUE_MAP));
  EXPECT_TRUE(sw.state);
  srv.post("fp/1/g/2", TRUE_MAP, sizeof(TRUE_MAP));
  EXPECT_FALSE(sw.state);
}

TEST(EntityCommands, SwitchToggleWithFalseDoesNothing) {
  TestSwitch sw;
  sw.configure("sw");
  CommandServer srv(ENTITYTYPE_SWITCH, &sw);

  srv.post("fp/1/g/2", FALSE_MAP, sizeof(FALSE_MAP));

  EXPECT_FALSE(sw.state);
}

TEST(EntityCommands, ResponseCarriesNewState) {
  TestSwitch sw;
  sw.configure("sw");
  CommandServer srv(ENTITYTYPE_SWITCH, &sw);

  srv.post("fp/1/g/1", TRUE_MAP, sizeof(TRUE_MAP));

  EXPECT_EQ(payload_of(srv.last_sent), std::string(reinterpret_cast<const char *>(TRUE_MAP), sizeof(TRUE_MAP)));
}

// ---------------------------------------------------------------------------
// Lock
// ---------------------------------------------------------------------------

TEST(EntityCommands, LockLock) {
  TestLock lk;
  lk.configure("lk");
  lk.state = lock::LOCK_STATE_UNLOCKED;
  CommandServer srv(ENTITYTYPE_LOCK, &lk);

  srv.post("fp/1/g/1", TRUE_MAP, sizeof(TRUE_MAP));

  EXPECT_EQ(lk.state, lock::LOCK_STATE_LOCKED);
}

TEST(EntityCommands, LockUnlock) {
  TestLock lk;
  lk.configure("lk");
  lk.state = lock::LOCK_STATE_LOCKED;
  CommandServer srv(ENTITYTYPE_LOCK, &lk);

  srv.post("fp/1/g/1", FALSE_MAP, sizeof(FALSE_MAP));

  EXPECT_EQ(lk.state, lock::LOCK_STATE_UNLOCKED);
}

// ---------------------------------------------------------------------------
// Valve
// ---------------------------------------------------------------------------

TEST(EntityCommands, ValveOpen) {
  TestValve v;
  v.configure("v");
  CommandServer srv(ENTITYTYPE_VALVE, &v);

  srv.post("fp/1/g/1", TRUE_MAP, sizeof(TRUE_MAP));

  ASSERT_TRUE(v.last_position.has_value());
  EXPECT_FLOAT_EQ(*v.last_position, valve::VALVE_OPEN);
  EXPECT_FALSE(v.last_stop);
}

TEST(EntityCommands, ValveClose) {
  TestValve v;
  v.configure("v");
  CommandServer srv(ENTITYTYPE_VALVE, &v);

  srv.post("fp/1/g/1", FALSE_MAP, sizeof(FALSE_MAP));

  ASSERT_TRUE(v.last_position.has_value());
  EXPECT_FLOAT_EQ(*v.last_position, valve::VALVE_CLOSED);
}

TEST(EntityCommands, ValveStop) {
  TestValve v;
  v.configure("v");
  CommandServer srv(ENTITYTYPE_VALVE, &v);

  srv.post("fp/1/g/2", TRUE_MAP, sizeof(TRUE_MAP));

  EXPECT_TRUE(v.last_stop);
  EXPECT_FALSE(v.last_position.has_value());
}

// ---------------------------------------------------------------------------
// Number: {2: value}
// ---------------------------------------------------------------------------

TEST(EntityCommands, NumberSetFloat) {
  TestNumber n;
  n.configure("n");
  CommandServer srv(ENTITYTYPE_NUMBER, &n);
  const uint8_t payload[] = {0xA1, 0x02, 0xFA, 0x42, 0x2A, 0x00, 0x00};  // {2: 42.5f}

  srv.post("fp/1/g/1", payload, sizeof(payload));

  EXPECT_FLOAT_EQ(n.last_control, 42.5f);
}

TEST(EntityCommands, NumberSetHalfFloat) {
  TestNumber n;
  n.configure("n");
  CommandServer srv(ENTITYTYPE_NUMBER, &n);
  const uint8_t payload[] = {0xA1, 0x02, 0xF9, 0x3E, 0x00};  // {2: 1.5} as a half-precision float

  srv.post("fp/1/g/1", payload, sizeof(payload));

  EXPECT_FLOAT_EQ(n.last_control, 1.5f);
}

TEST(EntityCommands, NumberSetInteger) {
  TestNumber n;
  n.configure("n");
  CommandServer srv(ENTITYTYPE_NUMBER, &n);
  const uint8_t payload[] = {0xA1, 0x02, 0x0A};  // {2: 10}

  srv.post("fp/1/g/1", payload, sizeof(payload));

  EXPECT_FLOAT_EQ(n.last_control, 10.0f);
}

TEST(EntityCommands, NumberSetNegativeInteger) {
  TestNumber n;
  n.configure("n");
  CommandServer srv(ENTITYTYPE_NUMBER, &n);
  const uint8_t payload[] = {0xA1, 0x02, 0x24};  // {2: -5}

  srv.post("fp/1/g/1", payload, sizeof(payload));

  EXPECT_FLOAT_EQ(n.last_control, -5.0f);
}

TEST(EntityCommands, NumberOutsideRangeIsRejectedByNumber) {
  TestNumber n;
  n.configure("n");
  n.traits.set_min_value(0.0f);
  n.traits.set_max_value(100.0f);
  CommandServer srv(ENTITYTYPE_NUMBER, &n);
  const uint8_t payload[] = {0xA1, 0x02, 0x18, 0xC8};  // {2: 200}

  srv.post("fp/1/g/1", payload, sizeof(payload));

  EXPECT_TRUE(std::isnan(n.last_control));
}

// ---------------------------------------------------------------------------
// Payloads that must not change anything
// ---------------------------------------------------------------------------

TEST(EntityCommands, UnknownKeyIgnored) {
  TestSwitch sw;
  sw.configure("sw");
  CommandServer srv(ENTITYTYPE_SWITCH, &sw);
  const uint8_t payload[] = {0xA1, 0x18, 0x63, 0xF5};  // {99: true}

  srv.post("fp/1/g/1", payload, sizeof(payload));

  EXPECT_FALSE(sw.state);
}

TEST(EntityCommands, WrongValueTypeIgnored) {
  TestSwitch sw;
  sw.configure("sw");
  CommandServer srv(ENTITYTYPE_SWITCH, &sw);
  const uint8_t payload[] = {0xA1, 0x04, 0x01};  // {4: 1}, not a boolean

  srv.post("fp/1/g/1", payload, sizeof(payload));

  EXPECT_FALSE(sw.state);
}

TEST(EntityCommands, NotAMapIgnored) {
  TestSwitch sw;
  sw.configure("sw");
  CommandServer srv(ENTITYTYPE_SWITCH, &sw);
  const uint8_t payload[] = {0x82, 0x04, 0xF5};  // [4, true]

  srv.post("fp/1/g/1", payload, sizeof(payload));

  EXPECT_FALSE(sw.state);
}

TEST(EntityCommands, TruncatedPayloadIgnored) {
  TestSwitch sw;
  sw.configure("sw");
  CommandServer srv(ENTITYTYPE_SWITCH, &sw);
  const uint8_t payload[] = {0xA1, 0x04};  // value missing

  srv.post("fp/1/g/1", payload, sizeof(payload));

  EXPECT_FALSE(sw.state);
}

TEST(EntityCommands, EmptyPayloadIgnored) {
  TestSwitch sw;
  sw.configure("sw");
  CommandServer srv(ENTITYTYPE_SWITCH, &sw);
  // POST without a payload marker
  std::vector<uint8_t> pkt{0x51, 0x02, 0x00, 0x02, 0xCD};
  append_uri_path(pkt, "fp/1/g/1", 0);

  srv.inject(pkt, make_peer());

  EXPECT_FALSE(sw.state);
  EXPECT_EQ(srv.response_code(), 0x44u);
}

TEST(EntityCommands, PostToSensorIsNotAllowed) {
  TestSensor s;
  s.configure("s");
  CommandServer srv(ENTITYTYPE_SENSOR, &s);

  srv.post("fp/1/g/1", TRUE_MAP, sizeof(TRUE_MAP));

  EXPECT_EQ(srv.response_code(), 0x85u);  // 4.05 Method Not Allowed
}

}  // namespace
}  // namespace esphome::coap_server::testing
