// Tests for the log resource: the CBOR entries on_log() appends and the payload take_log_payload_() hands out.
// Format: 0x9F [array(4) millis level tag message]* 0xFF

#include "common.h"

#ifdef USE_LOGGER

namespace esphome::coap_server::testing {
namespace {

bool read_cbor_uint(const uint8_t *buf, size_t len, size_t &pos, uint64_t &out) {
  if (pos >= len)
    return false;
  uint8_t info = buf[pos++] & 0x1fu;
  if (info <= 23u) {
    out = info;
    return true;
  }
  size_t bytes = info == 0x18u ? 1 : info == 0x19u ? 2 : info == 0x1au ? 4 : 0;
  if (bytes == 0 || pos + bytes > len)
    return false;
  out = 0;
  for (size_t i = 0; i < bytes; i++)
    out = (out << 8) | buf[pos++];
  return true;
}

bool read_cbor_text(const uint8_t *buf, size_t len, size_t &pos, std::string &out) {
  if (pos >= len || (buf[pos] >> 5u) != 3u)
    return false;
  uint64_t str_len = 0;
  if (!read_cbor_uint(buf, len, pos, str_len) || pos + str_len > len)
    return false;
  out.assign(reinterpret_cast<const char *>(buf + pos), static_cast<size_t>(str_len));
  pos += static_cast<size_t>(str_len);
  return true;
}

struct LogEntry {
  uint64_t millis;
  uint64_t level;
  std::string tag;
  std::string message;
};

std::vector<LogEntry> parse_log_buffer(const uint8_t *buf, size_t len) {
  std::vector<LogEntry> entries;
  size_t pos = 0;
  if (len == 0 || buf[pos++] != 0x9Fu)
    return entries;
  while (pos < len && buf[pos] != 0xFFu) {
    if (buf[pos++] != 0x84u)
      break;
    LogEntry e{};
    if (!read_cbor_uint(buf, len, pos, e.millis) || !read_cbor_uint(buf, len, pos, e.level) ||
        !read_cbor_text(buf, len, pos, e.tag) || !read_cbor_text(buf, len, pos, e.message))
      break;
    entries.push_back(e);
  }
  return entries;
}

class LogServer : public TestableCoapServerNet {
 public:
  // The log part of setup()
  LogServer() {
    this->init_resources(2);
    uint16_t idx = 1;
    this->add_resource(ENTITYTYPE_LOG, nullptr, true, idx);
    this->logs_resource_ = &this->resources_[this->resources_.size() - 1];
    this->log_buf_[0] = 0x9F;
    this->log_buf_pos_ = 1;
  }

  void subscribe() { this->inject(make_coap_get(this->logs_resource_->path, 1, 0), make_peer()); }
  const char *logs_path() const { return this->logs_resource_->path; }
  void unsubscribe() { this->inject(make_coap_get(this->logs_resource_->path, 2, 1), make_peer()); }
  void log(uint8_t level, const char *tag, const char *message) { this->on_log(level, tag, message, strlen(message)); }

  std::vector<LogEntry> buffered() const { return parse_log_buffer(this->log_buf_, this->log_buf_pos_); }
  size_t buffer_pos() const { return this->log_buf_pos_; }
  bool has_log_data() const { return this->log_buf_has_data_; }
  size_t take(uint8_t *out) { return this->take_log_payload_(out); }
  static constexpr size_t BUF_SIZE = LOG_BUF_SIZE;
};

TEST(LogCbor, SubscribeAnswersWithEmptyArray) {
  LogServer srv;
  srv.subscribe();

  ASSERT_FALSE(srv.last_sent.empty());
  EXPECT_EQ(srv.last_sent[1], 0x45u);
  EXPECT_EQ(payload_of(srv.last_sent), std::string(1, '\x80'));
  EXPECT_EQ(srv.active_observer_count(), 1u);
}

TEST(LogCbor, SubscribeAtCapAnsweredWithoutObserve) {
  LogServer srv;
  for (uint16_t port = 1; port <= USE_COAP_SERVER_MAX_CLIENTS; port++)
    srv.inject(make_coap_get(srv.logs_path(), port, 0), make_peer(port));

  srv.inject(make_coap_get(srv.logs_path(), 100, 0), make_peer(100));

  ASSERT_GT(srv.last_sent.size(), 5u);
  EXPECT_EQ(srv.last_sent[1], 0x45u);
  EXPECT_NE(srv.last_sent[5] >> 4, 6);  // first option is not Observe
  EXPECT_EQ(srv.active_observer_count(), USE_COAP_SERVER_MAX_CLIENTS);
}

TEST(LogCbor, NoObserverSkipsEntry) {
  LogServer srv;
  srv.log(1, "tag", "ignored");

  EXPECT_FALSE(srv.has_log_data());
  EXPECT_EQ(srv.buffer_pos(), 1u);
}

TEST(LogCbor, UnsubscribeStopsEntries) {
  LogServer srv;
  srv.subscribe();
  srv.unsubscribe();
  srv.log(1, "tag", "ignored");

  EXPECT_FALSE(srv.has_log_data());
}

TEST(LogCbor, EntryHasLevelTagAndMessage) {
  LogServer srv;
  srv.subscribe();
  srv.log(5, "mycomponent", "hello world");

  EXPECT_TRUE(srv.has_log_data());
  auto entries = srv.buffered();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].level, 5u);
  EXPECT_EQ(entries[0].tag, "mycomponent");
  EXPECT_EQ(entries[0].message, "hello world");
}

TEST(LogCbor, NullTagEncodesAsEmptyString) {
  LogServer srv;
  srv.subscribe();
  srv.log(1, nullptr, "msg");

  auto entries = srv.buffered();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].tag, "");
}

TEST(LogCbor, MultipleEntriesKeepOrder) {
  LogServer srv;
  srv.subscribe();
  srv.log(1, "t1", "first");
  srv.log(2, "t2", "second");
  srv.log(3, "t3", "third");

  auto entries = srv.buffered();
  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(entries[0].message, "first");
  EXPECT_EQ(entries[1].message, "second");
  EXPECT_EQ(entries[2].message, "third");
}

TEST(LogCbor, TakeClosesArrayAndResets) {
  LogServer srv;
  srv.subscribe();
  srv.log(3, "tag", "msg");
  size_t pos_before = srv.buffer_pos();

  uint8_t out[LogServer::BUF_SIZE];
  size_t len = srv.take(out);

  ASSERT_EQ(len, pos_before + 1);
  EXPECT_EQ(out[0], 0x9Fu);
  EXPECT_EQ(out[len - 1], 0xFFu);
  EXPECT_EQ(parse_log_buffer(out, len).size(), 1u);
  EXPECT_FALSE(srv.has_log_data());
  EXPECT_EQ(srv.buffer_pos(), 1u);
  EXPECT_EQ(srv.take(out), 0u);
}

TEST(LogCbor, LongMessageIsCut) {
  LogServer srv;
  srv.subscribe();
  const std::string message(400, 'm');
  srv.log(3, "tag", message.c_str());

  auto entries = srv.buffered();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_GT(entries[0].message.size(), 0u);
  EXPECT_LT(entries[0].message.size(), message.size());
  EXPECT_EQ(message.compare(0, entries[0].message.size(), entries[0].message), 0);
}

TEST(LogCbor, LongMessageIsCutAtCharacterBoundary) {
  LogServer srv;
  srv.subscribe();
  // One ASCII byte, then 2-byte characters, so some cut points fall inside a character
  std::string message = "x";
  for (int i = 0; i < 200; i++)
    message += "\xC3\xA9";  // é
  srv.log(3, "tag", message.c_str());

  auto entries = srv.buffered();
  ASSERT_EQ(entries.size(), 1u);
  const size_t len = entries[0].message.size();
  ASSERT_GT(len, 0u);
  ASSERT_LT(len, message.size());
  EXPECT_EQ(message.compare(0, len, entries[0].message), 0);
  EXPECT_NE(static_cast<uint8_t>(message[len]) & 0xC0, 0x80);  // the cut is not inside a character
}

TEST(LogCbor, LongMessageKeepsLaterEntriesReadable) {
  LogServer srv;
  srv.subscribe();
  const std::string message(400, 'm');
  srv.log(3, "tag", message.c_str());
  srv.log(3, "tag", "after");

  auto entries = srv.buffered();
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[1].message, "after");
}

TEST(LogCbor, FullBufferDropsNewEntries) {
  LogServer srv;
  srv.subscribe();
  const std::string message(200, 'm');
  for (int i = 0; i < 20; i++)
    srv.log(3, "tag", message.c_str());

  // Room is always left for the closing 0xFF
  EXPECT_LT(srv.buffer_pos(), LogServer::BUF_SIZE);
  auto entries = srv.buffered();
  EXPECT_GT(entries.size(), 0u);
  EXPECT_LT(entries.size(), 20u);
  for (const auto &e : entries)
    EXPECT_EQ(e.message, message);
}

}  // namespace
}  // namespace esphome::coap_server::testing

#endif  // USE_LOGGER
