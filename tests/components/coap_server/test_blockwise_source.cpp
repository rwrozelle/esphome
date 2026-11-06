// Tests for CoapServer::BlockwiseSource and blockwise_transmit_hook.

#include <gtest/gtest.h>
#include <cstring>

#include "esphome/components/coap_server/coap_server.h"

namespace esphome::coap_server::testing {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static CoapServer::BlockwiseSource make_source(const uint8_t *data, size_t len, uint16_t cf = 40) {
  CoapServer::BlockwiseSource src{};
  src.data = data;
  src.data_len = len;
  src.content_format = cf;
  return src;
}

// Call the hook and return the number of bytes written.
static uint16_t call_hook(CoapServer::BlockwiseSource *src, uint32_t position, uint8_t *out, uint16_t max_len,
                          bool *more) {
  uint16_t len = max_len;
  otError err = CoapServer::blockwise_transmit_hook(src, out, position, &len, more);
  return (err == OT_ERROR_NONE) ? len : 0;
}

// ---------------------------------------------------------------------------
// In-memory source tests
// ---------------------------------------------------------------------------

TEST(BlockwiseSource, FirstBlock_MoreTrue) {
  const uint8_t data[512]{};
  auto src = make_source(data, sizeof(data));
  uint8_t out[256];
  bool more = false;
  uint16_t len = call_hook(&src, 0, out, sizeof(out), &more);
  EXPECT_EQ(len, 256u);
  EXPECT_TRUE(more);
}

TEST(BlockwiseSource, LastBlock_MoreFalse) {
  const uint8_t data[512]{};
  auto src = make_source(data, sizeof(data));
  uint8_t out[256];
  bool more = true;
  uint16_t len = call_hook(&src, 256, out, sizeof(out), &more);
  EXPECT_EQ(len, 256u);
  EXPECT_FALSE(more);
}

TEST(BlockwiseSource, SingleBlock_FitsAll) {
  const uint8_t data[] = {1, 2, 3, 4, 5};
  auto src = make_source(data, sizeof(data));
  uint8_t out[256];
  bool more = true;
  uint16_t len = call_hook(&src, 0, out, sizeof(out), &more);
  EXPECT_EQ(len, 5u);
  EXPECT_FALSE(more);
  EXPECT_EQ(memcmp(out, data, 5), 0);
}

TEST(BlockwiseSource, PositionBeyondEnd_InvalidArgs) {
  const uint8_t data[10]{};
  auto src = make_source(data, sizeof(data));
  uint8_t out[256];
  bool more = false;
  uint16_t block_len = sizeof(out);
  otError err = CoapServer::blockwise_transmit_hook(&src, out, 10, &block_len, &more);
  EXPECT_EQ(err, OT_ERROR_INVALID_ARGS);
}

TEST(BlockwiseSource, PartialLastBlock_CorrectLength) {
  // 300 bytes total, 256-byte block size: first block=256, second block=44
  uint8_t data[300];
  for (int i = 0; i < 300; i++)
    data[i] = (uint8_t) i;
  auto src = make_source(data, sizeof(data));

  uint8_t out[256];
  bool more = true;

  // First block
  uint16_t len = call_hook(&src, 0, out, sizeof(out), &more);
  EXPECT_EQ(len, 256u);
  EXPECT_TRUE(more);
  EXPECT_EQ(memcmp(out, data, 256), 0);

  // Second (last) block
  len = call_hook(&src, 256, out, sizeof(out), &more);
  EXPECT_EQ(len, 44u);
  EXPECT_FALSE(more);
  EXPECT_EQ(memcmp(out, data + 256, 44), 0);
}

TEST(BlockwiseSource, DataIntegrity_ContentPreserved) {
  uint8_t data[512];
  for (int i = 0; i < 512; i++)
    data[i] = (uint8_t) (i & 0xFF);
  auto src = make_source(data, sizeof(data));

  uint8_t out[256];
  bool more;
  call_hook(&src, 0, out, sizeof(out), &more);
  EXPECT_EQ(memcmp(out, data, 256), 0);
  call_hook(&src, 256, out, sizeof(out), &more);
  EXPECT_EQ(memcmp(out, data + 256, 256), 0);
}

// ---------------------------------------------------------------------------
// read_fn dispatch test
// ---------------------------------------------------------------------------

struct ReadFnCtx {
  const uint8_t *data;
  size_t data_len;
  bool called{false};
};

static otError test_read_fn(void *ctx, uint8_t *buf, uint32_t pos, uint16_t *len, bool *more) {
  auto *c = static_cast<ReadFnCtx *>(ctx);
  c->called = true;
  if (pos >= c->data_len)
    return OT_ERROR_INVALID_ARGS;
  size_t avail = c->data_len - pos;
  uint16_t n = (uint16_t) std::min((size_t) *len, avail);
  memcpy(buf, c->data + pos, n);
  *len = n;
  *more = (pos + n) < c->data_len;
  return OT_ERROR_NONE;
}

TEST(BlockwiseSource, ReadFn_Dispatches) {
  const uint8_t data[] = {10, 20, 30};
  ReadFnCtx ctx{data, sizeof(data)};

  CoapServer::BlockwiseSource src{};
  src.read_fn = test_read_fn;
  src.read_ctx = &ctx;

  uint8_t out[256];
  bool more = true;
  uint16_t len = sizeof(out);
  otError err = CoapServer::blockwise_transmit_hook(&src, out, 0, &len, &more);

  EXPECT_EQ(err, OT_ERROR_NONE);
  EXPECT_TRUE(ctx.called);
  EXPECT_EQ(len, 3u);
  EXPECT_FALSE(more);
  EXPECT_EQ(memcmp(out, data, 3), 0);
}

TEST(BlockwiseSource, ReadFn_InMemoryIgnored_WhenReadFnSet) {
  // When read_fn is set, data/data_len should be ignored
  const uint8_t ignored[] = {0xFF, 0xFF};
  const uint8_t fn_data[] = {1, 2, 3};
  ReadFnCtx ctx{fn_data, sizeof(fn_data)};

  CoapServer::BlockwiseSource src{};
  src.data = ignored;
  src.data_len = sizeof(ignored);
  src.read_fn = test_read_fn;
  src.read_ctx = &ctx;

  uint8_t out[256];
  bool more;
  uint16_t len = sizeof(out);
  CoapServer::blockwise_transmit_hook(&src, out, 0, &len, &more);

  // Should have gotten fn_data, not ignored
  EXPECT_EQ(len, 3u);
  EXPECT_EQ(memcmp(out, fn_data, 3), 0);
}

}  // namespace esphome::coap_server::testing
