// Tests for the token and message init calls the OpenThread transport uses. Built as usual they test the fake
// OpenThread API; with COAP_SERVER_OT_API=440 they test ot_coap_compat.h, which provides them on NCS.

#include "common.h"

#ifdef USE_COAP_SERVER_OPENTHREAD

#include "esphome/components/coap_server/ot_coap_compat.h"

namespace esphome::coap_server::testing {
namespace {

otCoapToken make_token(std::initializer_list<uint8_t> bytes) {
  otCoapToken token{};
  for (uint8_t b : bytes)
    token.m8[token.mLength++] = b;
  return token;
}

TEST(CoapServerOTCompat, WrittenTokenReadsBack) {
  otMessage *message = otCoapNewMessage(nullptr, nullptr);
  const otCoapToken written = make_token({0x12, 0x34, 0x56, 0x78});
  ASSERT_EQ(otCoapMessageWriteToken(message, &written), OT_ERROR_NONE);

  otCoapToken read{};
  ASSERT_EQ(otCoapMessageReadToken(message, &read), OT_ERROR_NONE);
  EXPECT_EQ(read.mLength, 4);
  EXPECT_TRUE(otCoapMessageAreTokensEqual(&written, &read));
}

TEST(CoapServerOTCompat, EmptyTokenReadsBack) {
  otMessage *message = otCoapNewMessage(nullptr, nullptr);
  otCoapToken read = make_token({0xFF});
  ASSERT_EQ(otCoapMessageReadToken(message, &read), OT_ERROR_NONE);
  EXPECT_EQ(read.mLength, 0);
}

TEST(CoapServerOTCompat, MaxLengthTokenReadsBack) {
  otMessage *message = otCoapNewMessage(nullptr, nullptr);
  const otCoapToken written = make_token({1, 2, 3, 4, 5, 6, 7, 8});
  ASSERT_EQ(otCoapMessageWriteToken(message, &written), OT_ERROR_NONE);

  otCoapToken read{};
  ASSERT_EQ(otCoapMessageReadToken(message, &read), OT_ERROR_NONE);
  EXPECT_TRUE(otCoapMessageAreTokensEqual(&written, &read));
}

TEST(CoapServerOTCompat, TokensDifferingInLengthAreNotEqual) {
  const otCoapToken a = make_token({0x01, 0x02});
  const otCoapToken b = make_token({0x01, 0x02, 0x03});
  EXPECT_FALSE(otCoapMessageAreTokensEqual(&a, &b));
}

TEST(CoapServerOTCompat, TokensDifferingInBytesAreNotEqual) {
  const otCoapToken a = make_token({0x01, 0x02});
  const otCoapToken b = make_token({0x01, 0x03});
  EXPECT_FALSE(otCoapMessageAreTokensEqual(&a, &b));
}

TEST(CoapServerOTCompat, MessageInitSetsTypeAndCode) {
  otMessage *message = otCoapNewMessage(nullptr, nullptr);
  EXPECT_EQ(coap_message_init(message, OT_COAP_TYPE_CONFIRMABLE, OT_COAP_CODE_CONTENT), OT_ERROR_NONE);
  EXPECT_EQ(otCoapMessageGetType(message), OT_COAP_TYPE_CONFIRMABLE);
  EXPECT_EQ(otCoapMessageGetCode(message), OT_COAP_CODE_CONTENT);
}

}  // namespace
}  // namespace esphome::coap_server::testing

#endif  // USE_COAP_SERVER_OPENTHREAD
