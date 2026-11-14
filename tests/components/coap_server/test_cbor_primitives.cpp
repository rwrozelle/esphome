// Tests for the CBOR encoder and parser in cbor.h / cbor.cpp:
//   - cbor_encode_null
//   - cbor_value_get_double: half-precision (0xF9) all branches, double-precision (0xFB)
//   - Parser error paths: truncated buffers, wrong-type calls, advance-past-end

#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "esphome/components/coap_server/cbor.h"

namespace esphome::coap_server::testing {

// ---------------------------------------------------------------------------
// cbor_encode_null
// ---------------------------------------------------------------------------

TEST(CborNull, EncodesNullByte) {
  uint8_t buf[4]{};
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, sizeof(buf), 0);
  ASSERT_EQ(cbor_encode_null(&enc), CBOR_NO_ERROR);
  EXPECT_EQ(cbor_encoder_get_buffer_size(&enc, buf), 1u);
  EXPECT_EQ(buf[0], 0xF6u);
}

TEST(CborNull, NullInMap) {
  uint8_t buf[8]{};
  CborEncoder enc, map;
  cbor_encoder_init(&enc, buf, sizeof(buf), 0);
  cbor_encoder_create_map(&enc, &map, 1);
  cbor_encode_uint(&map, 1);
  cbor_encode_null(&map);
  cbor_encoder_close_container(&enc, &map);
  size_t len = cbor_encoder_get_buffer_size(&enc, buf);
  // {1: null} → 0xa1 0x01 0xf6
  ASSERT_EQ(len, 3u);
  EXPECT_EQ(buf[0], 0xa1u);
  EXPECT_EQ(buf[1], 0x01u);
  EXPECT_EQ(buf[2], 0xf6u);
}

TEST(CborNull, OverflowReturnsError) {
  uint8_t buf[0]{};
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, 0, 0);
  EXPECT_EQ(cbor_encode_null(&enc), CBOR_ERROR_OUT_OF_MEMORY);
}

// ---------------------------------------------------------------------------
// Half-precision float (0xF9) — decode_half_float branches
// ---------------------------------------------------------------------------

// Build a 3-byte CBOR half-precision item {0xF9, hi, lo} and parse it.
static double parse_half(uint16_t bits) {
  uint8_t buf[3] = {0xF9, (uint8_t) (bits >> 8), (uint8_t) (bits & 0xFF)};
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  double result = 0.0;
  EXPECT_EQ(cbor_value_get_double(&v, &result), CBOR_NO_ERROR);
  return result;
}

// Normal values
TEST(CborHalfFloat, One) { EXPECT_DOUBLE_EQ(parse_half(0x3C00), 1.0); }
TEST(CborHalfFloat, MinusOne) { EXPECT_DOUBLE_EQ(parse_half(0xBC00), -1.0); }
TEST(CborHalfFloat, Two) { EXPECT_DOUBLE_EQ(parse_half(0x4000), 2.0); }
TEST(CborHalfFloat, Half) { EXPECT_DOUBLE_EQ(parse_half(0x3800), 0.5); }
TEST(CborHalfFloat, Zero) { EXPECT_DOUBLE_EQ(parse_half(0x0000), 0.0); }

// Subnormal — exp=0: val = ldexp(mant, -24)
TEST(CborHalfFloat, SmallestSubnormal) { EXPECT_DOUBLE_EQ(parse_half(0x0001), std::ldexp(1.0, -24)); }
TEST(CborHalfFloat, LargestSubnormal) { EXPECT_DOUBLE_EQ(parse_half(0x03FF), std::ldexp(1023.0, -24)); }

// Infinity — exp=31, mant=0
TEST(CborHalfFloat, PositiveInfinity) {
  double v = parse_half(0x7C00);
  EXPECT_TRUE(std::isinf(v));
  EXPECT_GT(v, 0.0);
}
TEST(CborHalfFloat, NegativeInfinity) {
  double v = parse_half(0xFC00);
  EXPECT_TRUE(std::isinf(v));
  EXPECT_LT(v, 0.0);
}

// NaN — exp=31, mant!=0
TEST(CborHalfFloat, NaN) { EXPECT_TRUE(std::isnan(parse_half(0x7E00))); }

// Negative NaN
TEST(CborHalfFloat, NegativeNaN) { EXPECT_TRUE(std::isnan(parse_half(0xFE00))); }

// cbor_value_is_half_float predicate
TEST(CborHalfFloat, IsHalfFloatPredicate) {
  uint8_t buf[3] = {0xF9, 0x3C, 0x00};
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  EXPECT_TRUE(cbor_value_is_half_float(&v));
  EXPECT_FALSE(cbor_value_is_float(&v));
  EXPECT_FALSE(cbor_value_is_double(&v));
}

// ---------------------------------------------------------------------------
// Double-precision (0xFB) — cbor_value_get_double
// ---------------------------------------------------------------------------

static double parse_double64(double val) {
  uint64_t bits;
  memcpy(&bits, &val, 8);
  uint8_t buf[9] = {
      0xFB,
      (uint8_t) (bits >> 56),
      (uint8_t) (bits >> 48),
      (uint8_t) (bits >> 40),
      (uint8_t) (bits >> 32),
      (uint8_t) (bits >> 24),
      (uint8_t) (bits >> 16),
      (uint8_t) (bits >> 8),
      (uint8_t) bits,
  };
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  double result = 0.0;
  EXPECT_EQ(cbor_value_get_double(&v, &result), CBOR_NO_ERROR);
  return result;
}

TEST(CborDoubleFloat, OnePointFive) { EXPECT_DOUBLE_EQ(parse_double64(1.5), 1.5); }
TEST(CborDoubleFloat, Pi) { EXPECT_DOUBLE_EQ(parse_double64(3.14159265358979), 3.14159265358979); }
TEST(CborDoubleFloat, Negative) { EXPECT_DOUBLE_EQ(parse_double64(-42.0), -42.0); }

TEST(CborDoubleFloat, IsDoublePredicate) {
  double val = 1.5;
  uint64_t bits;
  memcpy(&bits, &val, 8);
  uint8_t buf[9] = {0xFB,
                    (uint8_t) (bits >> 56),
                    (uint8_t) (bits >> 48),
                    (uint8_t) (bits >> 40),
                    (uint8_t) (bits >> 32),
                    (uint8_t) (bits >> 24),
                    (uint8_t) (bits >> 16),
                    (uint8_t) (bits >> 8),
                    (uint8_t) bits};
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  EXPECT_TRUE(cbor_value_is_double(&v));
  EXPECT_FALSE(cbor_value_is_float(&v));
  EXPECT_FALSE(cbor_value_is_half_float(&v));
}

// Single-precision round-trip through encoder → parser
TEST(CborDoubleFloat, SinglePrecisionRoundTrip) {
  float f = 3.14f;
  uint8_t buf[5]{};
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, sizeof(buf), 0);
  cbor_encode_float(&enc, f);

  CborParser p;
  CborValue v;
  cbor_parser_init(buf, 5, 0, &p, &v);
  double result = 0.0;
  ASSERT_EQ(cbor_value_get_double(&v, &result), CBOR_NO_ERROR);
  EXPECT_FLOAT_EQ((float) result, f);
}

// ---------------------------------------------------------------------------
// Parser error paths — truncated buffers
// ---------------------------------------------------------------------------

TEST(CborParserErrors, TruncatedHalfFloat) {
  uint8_t buf[] = {0xF9, 0x3C};  // need 3 bytes, only 2
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  double result;
  EXPECT_NE(cbor_value_get_double(&v, &result), CBOR_NO_ERROR);
}

TEST(CborParserErrors, TruncatedSingleFloat) {
  uint8_t buf[] = {0xFA, 0x3F, 0xC0};  // need 5 bytes, only 3
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  double result;
  EXPECT_NE(cbor_value_get_double(&v, &result), CBOR_NO_ERROR);
}

TEST(CborParserErrors, TruncatedDoubleFloat) {
  uint8_t buf[] = {0xFB, 0x3F, 0xF8};  // need 9 bytes, only 3
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  double result;
  EXPECT_NE(cbor_value_get_double(&v, &result), CBOR_NO_ERROR);
}

// ---------------------------------------------------------------------------
// Parser error paths — wrong-type calls
// ---------------------------------------------------------------------------

TEST(CborParserErrors, GetDoubleOnInteger) {
  uint8_t buf[] = {0x01};  // integer 1
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  double result;
  EXPECT_NE(cbor_value_get_double(&v, &result), CBOR_NO_ERROR);
}

TEST(CborParserErrors, GetDoubleOnBoolean) {
  uint8_t buf[] = {0xF5};  // true
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  double result;
  EXPECT_NE(cbor_value_get_double(&v, &result), CBOR_NO_ERROR);
}

TEST(CborParserErrors, GetIntOnBoolean) {
  uint8_t buf[] = {0xF5};  // true
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  int result;
  EXPECT_NE(cbor_value_get_int(&v, &result), CBOR_NO_ERROR);
}

TEST(CborParserErrors, GetBooleanOnInteger) {
  uint8_t buf[] = {0x01};  // integer 1
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  bool result;
  EXPECT_NE(cbor_value_get_boolean(&v, &result), CBOR_NO_ERROR);
}

TEST(CborParserErrors, GetBooleanOnFloat) {
  uint8_t buf[] = {0xFA, 0x3F, 0x80, 0x00, 0x00};  // single 1.0f
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  bool result;
  EXPECT_NE(cbor_value_get_boolean(&v, &result), CBOR_NO_ERROR);
}

TEST(CborParserErrors, EnterContainerOnInteger) {
  uint8_t buf[] = {0x01};
  CborParser p;
  CborValue v, child;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  EXPECT_NE(cbor_value_enter_container(&v, &child), CBOR_NO_ERROR);
}

TEST(CborParserErrors, EnterContainerOnBoolean) {
  uint8_t buf[] = {0xF5};
  CborParser p;
  CborValue v, child;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  EXPECT_NE(cbor_value_enter_container(&v, &child), CBOR_NO_ERROR);
}

// ---------------------------------------------------------------------------
// Parser error paths — advance and at-end
// ---------------------------------------------------------------------------

TEST(CborParserErrors, AdvancePastEnd) {
  uint8_t buf[] = {0x01};  // single integer — remaining starts at 1
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  ASSERT_EQ(cbor_value_advance(&v), CBOR_NO_ERROR);  // consumes the one item → remaining=0
  EXPECT_NE(cbor_value_advance(&v), CBOR_NO_ERROR);  // remaining=0 → error
}

TEST(CborParserErrors, AdvanceTruncatedString) {
  uint8_t buf[] = {0x63, 'a', 'b'};  // text string of 3 bytes with only 2 present
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  EXPECT_NE(cbor_value_advance(&v), CBOR_NO_ERROR);
  EXPECT_EQ(v.ptr, buf);
}

TEST(CborParserErrors, AdvanceStringLengthThatWrapsPointer) {
  // byte string whose 64-bit length makes ptr + length wrap around to before the buffer
  uint8_t buf[] = {0x5B, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF0};
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  EXPECT_NE(cbor_value_advance(&v), CBOR_NO_ERROR);
  EXPECT_EQ(v.ptr, buf);
}

TEST(CborParserErrors, AdvanceNestedToLimit) {
  uint8_t buf[] = {0x81, 0x01};  // [1], 1 level
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  EXPECT_EQ(cbor_value_advance(&v), CBOR_NO_ERROR);
  EXPECT_EQ(v.ptr, buf + sizeof(buf));
}

TEST(CborParserErrors, AdvanceArrayNestedTooDeep) {
  uint8_t buf[] = {0x81, 0x81, 0x01};  // [[1]], 2 levels
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  EXPECT_EQ(cbor_value_advance(&v), CBOR_ERROR_NESTING_TOO_DEEP);
  EXPECT_EQ(v.ptr, buf);
}

TEST(CborParserErrors, AdvanceMapNestedTooDeep) {
  uint8_t buf[] = {0xA1, 0x01, 0xA1, 0x01, 0x01};  // {1: {1: 1}}, 2 levels
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  EXPECT_EQ(cbor_value_advance(&v), CBOR_ERROR_NESTING_TOO_DEEP);
  EXPECT_EQ(v.ptr, buf);
}

TEST(CborParserErrors, AdvanceMapCountThatOverflows) {
  // map with 2^63 pairs: 2 * count wraps to 0, which used to skip it as an empty map
  uint8_t buf[] = {0xBB, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  EXPECT_EQ(cbor_value_advance(&v), CBOR_ERROR_UNEXPECTED_EOF);
  EXPECT_EQ(v.ptr, buf);
}

TEST(CborParserErrors, AdvanceArrayCountLargerThanData) {
  uint8_t buf[] = {0x9A, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};  // array of 2^32 - 1 items, one present
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  EXPECT_EQ(cbor_value_advance(&v), CBOR_ERROR_UNEXPECTED_EOF);
  EXPECT_EQ(v.ptr, buf);
}

TEST(CborParserErrors, AdvanceMapSkipsPairs) {
  uint8_t buf[] = {0xA2, 0x01, 0xF5, 0x02, 0xF4, 0x07};  // {1: true, 2: false} then 7
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  ASSERT_EQ(cbor_value_advance(&v), CBOR_NO_ERROR);
  EXPECT_EQ(v.ptr, buf + 5);
}

TEST(CborParserErrors, EnterContainerCountLargerThanData) {
  uint8_t buf[] = {0xBB, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // map with 2^63 pairs
  CborParser p;
  CborValue v, child;
  cbor_parser_init(buf, sizeof(buf), 0, &p, &v);
  EXPECT_EQ(cbor_value_enter_container(&v, &child), CBOR_ERROR_UNEXPECTED_EOF);
}

TEST(CborParserErrors, GetInt64Limits) {
  CborParser p;
  CborValue v;
  int64_t result;
  uint8_t max_pos[] = {0x1B, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // INT64_MAX
  cbor_parser_init(max_pos, sizeof(max_pos), 0, &p, &v);
  ASSERT_EQ(cbor_value_get_int64(&v, &result), CBOR_NO_ERROR);
  EXPECT_EQ(result, INT64_MAX);
  uint8_t max_neg[] = {0x3B, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // INT64_MIN
  cbor_parser_init(max_neg, sizeof(max_neg), 0, &p, &v);
  ASSERT_EQ(cbor_value_get_int64(&v, &result), CBOR_NO_ERROR);
  EXPECT_EQ(result, INT64_MIN);
}

TEST(CborParserErrors, GetInt64TooLarge) {
  CborParser p;
  CborValue v;
  int64_t result;
  uint8_t too_pos[] = {0x1B, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // 2^63
  cbor_parser_init(too_pos, sizeof(too_pos), 0, &p, &v);
  EXPECT_EQ(cbor_value_get_int64(&v, &result), CBOR_ERROR_DATA_TOO_LARGE);
  uint8_t too_neg[] = {0x3B, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // -1 - 2^63
  cbor_parser_init(too_neg, sizeof(too_neg), 0, &p, &v);
  EXPECT_EQ(cbor_value_get_int64(&v, &result), CBOR_ERROR_DATA_TOO_LARGE);
}

TEST(CborParserErrors, GetIntOutOfRange) {
  CborParser p;
  CborValue v;
  int result = 0;
  uint8_t wraps_to_4[] = {0x1B, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04};  // 2^32 + 4
  cbor_parser_init(wraps_to_4, sizeof(wraps_to_4), 0, &p, &v);
  EXPECT_EQ(cbor_value_get_int(&v, &result), CBOR_ERROR_DATA_TOO_LARGE);
  uint8_t below_min[] = {0x3A, 0x80, 0x00, 0x00, 0x00};  // INT_MIN - 1
  cbor_parser_init(below_min, sizeof(below_min), 0, &p, &v);
  EXPECT_EQ(cbor_value_get_int(&v, &result), CBOR_ERROR_DATA_TOO_LARGE);
  uint8_t at_min[] = {0x3A, 0x7F, 0xFF, 0xFF, 0xFF};  // INT_MIN
  cbor_parser_init(at_min, sizeof(at_min), 0, &p, &v);
  ASSERT_EQ(cbor_value_get_int(&v, &result), CBOR_NO_ERROR);
  EXPECT_EQ(result, INT_MIN);
}

TEST(CborParserErrors, AtEndOnEmptyBuffer) {
  uint8_t buf[1]{};
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, 0, 0, &p, &v);  // zero-length → ptr == end
  EXPECT_TRUE(cbor_value_at_end(&v));
}

TEST(CborParserErrors, GetIntOnEmptyBuffer) {
  uint8_t buf[1]{};
  CborParser p;
  CborValue v;
  cbor_parser_init(buf, 0, 0, &p, &v);
  int result;
  EXPECT_NE(cbor_value_get_int(&v, &result), CBOR_NO_ERROR);
}
}  // namespace esphome::coap_server::testing
