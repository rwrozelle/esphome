// Tests for the per-entity CBOR (SenML) encoding in coap_server_cbor.cpp.

#include <limits>

#include "common.h"

namespace esphome::coap_server::testing {
namespace {

class CborServer : public TestableCoapServerNet {
 public:
  size_t encode(uint8_t *buf, size_t buf_len, EntityBase *entity, EntityType type) {
    return this->cbor_output_(buf, buf_len, entity, type);
  }
};

float decoded_float(const uint8_t *buf, size_t offset) {
  uint32_t bits = (static_cast<uint32_t>(buf[offset]) << 24) | (static_cast<uint32_t>(buf[offset + 1]) << 16) |
                  (static_cast<uint32_t>(buf[offset + 2]) << 8) | buf[offset + 3];
  float val;
  memcpy(&val, &bits, sizeof(val));
  return val;
}

// ---------------------------------------------------------------------------
// Sensor
// ---------------------------------------------------------------------------

TEST(CborOutput, SensorFloat) {
  CborServer srv;
  uint8_t buf[32]{};
  TestSensor s;
  s.state = 1.5f;
  size_t len = srv.encode(buf, sizeof(buf), &s, ENTITYTYPE_SENSOR);
  // {2: 1.5f}
  ASSERT_EQ(len, 7u);
  EXPECT_EQ(buf[0], 0xa1u);
  EXPECT_EQ(buf[1], 0x02u);
  EXPECT_EQ(buf[2], 0xfau);
  EXPECT_FLOAT_EQ(decoded_float(buf, 3), 1.5f);
}

TEST(CborOutput, SensorNaN) {
  CborServer srv;
  uint8_t buf[32]{};
  TestSensor s;
  s.state = std::numeric_limits<float>::quiet_NaN();
  size_t len = srv.encode(buf, sizeof(buf), &s, ENTITYTYPE_SENSOR);
  // {3: "NA"}
  const uint8_t expected[] = {0xa1, 0x03, 0x62, 'N', 'A'};
  ASSERT_EQ(len, sizeof(expected));
  EXPECT_EQ(memcmp(buf, expected, sizeof(expected)), 0);
}

TEST(CborOutput, SensorWithUnit) {
  uint8_t uom = string_table_index(entity_uom_lookup, "°C");
  ASSERT_NE(uom, 0u) << "unit not registered by the test codegen";
  CborServer srv;
  uint8_t buf[32]{};
  TestSensor s;
  s.configure("temp", 0, static_cast<uint32_t>(uom) << ENTITY_FIELD_UOM_SHIFT);
  s.state = 1.5f;
  size_t len = srv.encode(buf, sizeof(buf), &s, ENTITYTYPE_SENSOR);
  // {2: 1.5f, 1: "°C"}
  const uint8_t expected_tail[] = {0x01, 0x63, 0xc2, 0xb0, 'C'};
  ASSERT_EQ(len, 7u + sizeof(expected_tail));
  EXPECT_EQ(buf[0], 0xa2u);
  EXPECT_FLOAT_EQ(decoded_float(buf, 3), 1.5f);
  EXPECT_EQ(memcmp(buf + 7, expected_tail, sizeof(expected_tail)), 0);
}

// ---------------------------------------------------------------------------
// Switch and binary sensor
// ---------------------------------------------------------------------------

TEST(CborOutput, SwitchTrue) {
  CborServer srv;
  uint8_t buf[8]{};
  TestSwitch sw;
  sw.state = true;
  size_t len = srv.encode(buf, sizeof(buf), &sw, ENTITYTYPE_SWITCH);
  // {4: true}
  const uint8_t expected[] = {0xa1, 0x04, 0xf5};
  ASSERT_EQ(len, sizeof(expected));
  EXPECT_EQ(memcmp(buf, expected, sizeof(expected)), 0);
}

TEST(CborOutput, SwitchFalse) {
  CborServer srv;
  uint8_t buf[8]{};
  TestSwitch sw;
  sw.state = false;
  size_t len = srv.encode(buf, sizeof(buf), &sw, ENTITYTYPE_SWITCH);
  ASSERT_EQ(len, 3u);
  EXPECT_EQ(buf[2], 0xf4u);
}

TEST(CborOutput, BinarySensorTrue) {
  CborServer srv;
  uint8_t buf[8]{};
  TestBinarySensor bs;
  bs.state = true;
  size_t len = srv.encode(buf, sizeof(buf), &bs, ENTITYTYPE_BINARY_SENSOR);
  ASSERT_EQ(len, 3u);
  EXPECT_EQ(buf[1], 0x04u);
  EXPECT_EQ(buf[2], 0xf5u);
}

// ---------------------------------------------------------------------------
// Text sensor
// ---------------------------------------------------------------------------

TEST(CborOutput, TextSensorValue) {
  CborServer srv;
  uint8_t buf[32]{};
  TestTextSensor ts;
  ts.state = "hi";
  size_t len = srv.encode(buf, sizeof(buf), &ts, ENTITYTYPE_TEXT_SENSOR);
  // {3: "hi"}
  const uint8_t expected[] = {0xa1, 0x03, 0x62, 'h', 'i'};
  ASSERT_EQ(len, sizeof(expected));
  EXPECT_EQ(memcmp(buf, expected, sizeof(expected)), 0);
}

TEST(CborOutput, TextSensorEmptyIsNA) {
  CborServer srv;
  uint8_t buf[16]{};
  TestTextSensor ts;
  size_t len = srv.encode(buf, sizeof(buf), &ts, ENTITYTYPE_TEXT_SENSOR);
  const uint8_t expected[] = {0xa1, 0x03, 0x62, 'N', 'A'};
  ASSERT_EQ(len, sizeof(expected));
  EXPECT_EQ(memcmp(buf, expected, sizeof(expected)), 0);
}

// ---------------------------------------------------------------------------
// Number
// ---------------------------------------------------------------------------

TEST(CborOutput, NumberFloat) {
  CborServer srv;
  uint8_t buf[32]{};
  TestNumber n;
  n.state = 0.5f;
  size_t len = srv.encode(buf, sizeof(buf), &n, ENTITYTYPE_NUMBER);
  ASSERT_EQ(len, 7u);
  EXPECT_EQ(buf[1], 0x02u);
  EXPECT_FLOAT_EQ(decoded_float(buf, 3), 0.5f);
}

TEST(CborOutput, NumberNaN) {
  CborServer srv;
  uint8_t buf[32]{};
  TestNumber n;
  n.state = NAN;
  size_t len = srv.encode(buf, sizeof(buf), &n, ENTITYTYPE_NUMBER);
  const uint8_t expected[] = {0xa1, 0x03, 0x62, 'N', 'A'};
  ASSERT_EQ(len, sizeof(expected));
  EXPECT_EQ(memcmp(buf, expected, sizeof(expected)), 0);
}

// ---------------------------------------------------------------------------
// Lock: {2: LockState}
// ---------------------------------------------------------------------------

class CborLockState : public ::testing::TestWithParam<lock::LockState> {};

TEST_P(CborLockState, EncodesStateAsUint) {
  CborServer srv;
  uint8_t buf[8]{};
  TestLock lk;
  lk.state = GetParam();
  size_t len = srv.encode(buf, sizeof(buf), &lk, ENTITYTYPE_LOCK);
  ASSERT_EQ(len, 3u);
  EXPECT_EQ(buf[0], 0xa1u);
  EXPECT_EQ(buf[1], 0x02u);
  EXPECT_EQ(buf[2], static_cast<uint8_t>(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(AllStates, CborLockState,
                         ::testing::Values(lock::LOCK_STATE_NONE, lock::LOCK_STATE_LOCKED, lock::LOCK_STATE_UNLOCKED,
                                           lock::LOCK_STATE_JAMMED, lock::LOCK_STATE_LOCKING,
                                           lock::LOCK_STATE_UNLOCKING, lock::LOCK_STATE_OPENING,
                                           lock::LOCK_STATE_OPEN));

// ---------------------------------------------------------------------------
// Valve: {2: position}
// ---------------------------------------------------------------------------

TEST(CborOutput, ValveHalfOpen) {
  CborServer srv;
  uint8_t buf[32]{};
  TestValve v;
  v.position = 0.5f;
  size_t len = srv.encode(buf, sizeof(buf), &v, ENTITYTYPE_VALVE);
  ASSERT_EQ(len, 7u);
  EXPECT_EQ(buf[1], 0x02u);
  EXPECT_FLOAT_EQ(decoded_float(buf, 3), 0.5f);
}

TEST(CborOutput, ValveClosed) {
  CborServer srv;
  uint8_t buf[32]{};
  TestValve v;
  v.position = 0.0f;
  size_t len = srv.encode(buf, sizeof(buf), &v, ENTITYTYPE_VALVE);
  ASSERT_EQ(len, 7u);
  EXPECT_FLOAT_EQ(decoded_float(buf, 3), 0.0f);
}

// ---------------------------------------------------------------------------
// Types without a state, and a buffer that is too small
// ---------------------------------------------------------------------------

TEST(CborOutput, ButtonReturnsZero) {
  CborServer srv;
  uint8_t buf[8]{};
  TestButton btn;
  EXPECT_EQ(srv.encode(buf, sizeof(buf), &btn, ENTITYTYPE_BUTTON), 0u);
}

TEST(CborOutput, DeviceReturnsZero) {
  CborServer srv;
  uint8_t buf[8]{};
  EXPECT_EQ(srv.encode(buf, sizeof(buf), nullptr, ENTITYTYPE_DEVICE), 0u);
}

TEST(CborOutput, BufferTooSmallReturnsZero) {
  CborServer srv;
  uint8_t buf[4]{};
  TestTextSensor ts;
  ts.configure("long");
  ts.state = "longer than the buffer";
  EXPECT_EQ(srv.encode(buf, sizeof(buf), &ts, ENTITYTYPE_TEXT_SENSOR), 0u);
}

}  // namespace
}  // namespace esphome::coap_server::testing
