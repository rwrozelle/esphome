// Tests for .well-known/core Block2 handling in the Net transport (RFC 7959).

#include "common.h"

namespace esphome::coap_server::testing {
namespace {

using CoapPacket = CoapServerNet::CoapPacket;

// Option value in the fewest big-endian bytes; 0 still takes one byte here
std::vector<uint8_t> encode_option_uint(uint32_t val) {
  std::vector<uint8_t> v;
  if (val > 0xFFFF)
    v.push_back(static_cast<uint8_t>(val >> 16));
  if (val > 0xFF)
    v.push_back(static_cast<uint8_t>(val >> 8));
  v.push_back(static_cast<uint8_t>(val & 0xFF));
  return v;
}

// NON GET for .well-known/core; a negative block2_num leaves the Block2 option out
std::vector<uint8_t> make_wk_get(int block2_num = -1, uint8_t block2_szx = 6) {
  auto pkt = make_coap_get(".well-known/core");
  if (block2_num >= 0) {
    auto value = encode_option_uint((static_cast<uint32_t>(block2_num) << 4) | (block2_szx & 0x07));
    pkt.push_back(static_cast<uint8_t>(((23 - 11) << 4) | value.size()));  // Block2 (23) after Uri-Path (11)
    pkt.insert(pkt.end(), value.begin(), value.end());
  }
  return pkt;
}

// Reads the Block2 option of a response; false if it has none
bool parse_response_block2(const std::vector<uint8_t> &resp, uint32_t *num, bool *more, uint8_t *szx) {
  CoapPacket pkt{};
  if (!CoapServerNet::parse_coap(resp.data(), resp.size(), &pkt) || !pkt.block2_present)
    return false;
  *num = pkt.block2_num;
  *szx = pkt.block2_szx;
  // parse_coap() drops the M bit, so read it from the raw option value
  size_t pos = 4 + (resp[0] & 0x0F);
  uint16_t opt_num = 0;
  while (pos < resp.size() && resp[pos] != 0xFF) {
    uint8_t b = resp[pos++];
    opt_num += (b >> 4) & 0x0F;
    uint8_t len = b & 0x0F;
    if (opt_num == 23) {
      *more = len > 0 && (resp[pos + len - 1] & 0x08) != 0;
      return true;
    }
    pos += len;
  }
  return false;
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
    content += "/g/1>;rt=\"sensor\"";
  }
  return content;
}

// ---------------------------------------------------------------------------
// parse_coap: Block2 option
// ---------------------------------------------------------------------------

TEST(BlockwiseNetParse, NoBlock2FlagFalse) {
  auto pkt = make_wk_get();
  CoapPacket parsed{};
  ASSERT_TRUE(CoapServerNet::parse_coap(pkt.data(), pkt.size(), &parsed));
  EXPECT_FALSE(parsed.block2_present);
}

TEST(BlockwiseNetParse, Block2Num0Szx6) {
  auto pkt = make_wk_get(0, 6);
  CoapPacket parsed{};
  ASSERT_TRUE(CoapServerNet::parse_coap(pkt.data(), pkt.size(), &parsed));
  EXPECT_TRUE(parsed.block2_present);
  EXPECT_EQ(parsed.block2_num, 0u);
  EXPECT_EQ(parsed.block2_szx, 6u);
}

TEST(BlockwiseNetParse, Block2Num1Szx6) {
  auto pkt = make_wk_get(1, 6);
  CoapPacket parsed{};
  ASSERT_TRUE(CoapServerNet::parse_coap(pkt.data(), pkt.size(), &parsed));
  EXPECT_TRUE(parsed.block2_present);
  EXPECT_EQ(parsed.block2_num, 1u);
  EXPECT_EQ(parsed.block2_szx, 6u);
}

TEST(BlockwiseNetParse, Block2Num16TwoBytes) {
  auto pkt = make_wk_get(16, 6);  // 0x106 needs two bytes
  CoapPacket parsed{};
  ASSERT_TRUE(CoapServerNet::parse_coap(pkt.data(), pkt.size(), &parsed));
  EXPECT_TRUE(parsed.block2_present);
  EXPECT_EQ(parsed.block2_num, 16u);
  EXPECT_EQ(parsed.block2_szx, 6u);
}

TEST(BlockwiseNetParse, Block2Szx4) {
  auto pkt = make_wk_get(0, 4);
  CoapPacket parsed{};
  ASSERT_TRUE(CoapServerNet::parse_coap(pkt.data(), pkt.size(), &parsed));
  EXPECT_TRUE(parsed.block2_present);
  EXPECT_EQ(parsed.block2_szx, 4u);
}

// ---------------------------------------------------------------------------
// handle_well_known_core_
// ---------------------------------------------------------------------------

TEST(BlockwiseNet, PlainGetSmallPayloadHasBlock2) {
  TestableCoapServerNet srv;
  const std::string content = "</fp/1/g/1>;rt=\"sensor\"";
  srv.set_link_format(content);

  srv.inject(make_wk_get(), make_peer());

  ASSERT_FALSE(srv.last_sent.empty());
  EXPECT_EQ(srv.last_sent[1], 0x45);
  uint32_t num = 99;
  bool more = true;
  uint8_t szx = 0;
  ASSERT_TRUE(parse_response_block2(srv.last_sent, &num, &more, &szx));
  EXPECT_EQ(num, 0u);
  EXPECT_FALSE(more);
  EXPECT_EQ(szx, 6u);
  EXPECT_EQ(payload_of(srv.last_sent), content);
}

TEST(BlockwiseNet, PlainGetLargePayloadOnlyFirstBlock) {
  TestableCoapServerNet srv;
  const std::string content = make_link_format(50);
  ASSERT_GT(content.size(), 1024u);
  srv.set_link_format(content);

  srv.inject(make_wk_get(), make_peer());

  ASSERT_FALSE(srv.last_sent.empty());
  EXPECT_EQ(srv.last_sent[1], 0x45);
  uint32_t num = 99;
  bool more = false;
  uint8_t szx = 0;
  ASSERT_TRUE(parse_response_block2(srv.last_sent, &num, &more, &szx));
  EXPECT_EQ(num, 0u);
  EXPECT_TRUE(more);
  EXPECT_EQ(payload_of(srv.last_sent), content.substr(0, 1024));
}

TEST(BlockwiseNet, Block2Num1ReturnsSecondBlock) {
  TestableCoapServerNet srv;
  const std::string content = make_link_format(50);
  srv.set_link_format(content);

  srv.inject(make_wk_get(1, 6), make_peer());

  ASSERT_FALSE(srv.last_sent.empty());
  EXPECT_EQ(srv.last_sent[1], 0x45);
  uint32_t num = 99;
  bool more = true;
  uint8_t szx = 0;
  ASSERT_TRUE(parse_response_block2(srv.last_sent, &num, &more, &szx));
  EXPECT_EQ(num, 1u);
  EXPECT_FALSE(more);
  EXPECT_EQ(payload_of(srv.last_sent), content.substr(1024));
}

TEST(BlockwiseNet, Block2OutOfRangeReturns400) {
  TestableCoapServerNet srv;
  srv.set_link_format("</fp/1/g/1>;rt=\"sensor\"");

  srv.inject(make_wk_get(99, 6), make_peer());

  ASSERT_FALSE(srv.last_sent.empty());
  EXPECT_EQ(srv.last_sent[1], 0x80);
}

TEST(BlockwiseNet, Block2Szx4HonorsSmallBlockSize) {
  TestableCoapServerNet srv;
  srv.set_link_format(std::string(600, 'x'));

  srv.inject(make_wk_get(0, 4), make_peer());

  ASSERT_FALSE(srv.last_sent.empty());
  uint32_t num = 99;
  bool more = false;
  uint8_t szx = 0;
  ASSERT_TRUE(parse_response_block2(srv.last_sent, &num, &more, &szx));
  EXPECT_EQ(szx, 4u);
  EXPECT_TRUE(more);
  EXPECT_EQ(payload_of(srv.last_sent).size(), 256u);
}

TEST(BlockwiseNet, PostGetsNoResponse) {
  TestableCoapServerNet srv;
  srv.set_link_format("</fp/1/g/1>;rt=\"sensor\"");

  uint8_t payload[] = {0xA0};
  srv.inject(make_coap_post(".well-known/core", payload, sizeof(payload)), make_peer());

  EXPECT_TRUE(srv.last_sent.empty());
}

}  // namespace
}  // namespace esphome::coap_server::testing
