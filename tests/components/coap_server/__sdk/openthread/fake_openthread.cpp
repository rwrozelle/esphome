// Fake OpenThread CoAP API for the coap_server unit tests: messages live in a small in-memory pool and
// responses are recorded instead of sent. Only what coap_server_ot.cpp calls is implemented.

#include "openthread/coap.h"
#include "openthread/fake_openthread.h"
#include "openthread/ip6.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

struct FakeMessage {
  uint8_t buf[2048];
  uint16_t len{0};
  otCoapCode code{OT_COAP_CODE_GET};
  otCoapType type{OT_COAP_TYPE_NON_CONFIRMABLE};
  uint8_t token[OT_COAP_MAX_TOKEN_LENGTH]{};
  uint8_t token_len{0};
  bool has_payload_marker{false};
  bool has_observe{false};
  uint32_t observe_value{0};
};

// otMessageFree() does nothing, so messages come from a small ring that is reused
FakeMessage g_messages[8];
int g_message_next = 0;

FakeMessage *alloc_message() {
  FakeMessage *m = &g_messages[g_message_next % 8];
  g_message_next++;
  *m = FakeMessage{};
  return m;
}

FakeMessage *as_fake(otMessage *msg) { return reinterpret_cast<FakeMessage *>(msg); }
const FakeMessage *as_fake(const otMessage *msg) { return reinterpret_cast<const FakeMessage *>(msg); }

uint8_t g_last_response[4096];
uint16_t g_last_response_len = 0;
int g_last_response_code = 0;
bool g_inline_send_called = false;
otCoapBlockwiseTransmitHook g_blockwise_hook = nullptr;
void *g_blockwise_ctx = nullptr;
bool g_response_has_block2 = false;
uint32_t g_response_block2_num = 0;
bool g_response_block2_more = false;
int g_response_block2_szx = 0;
int g_request_block2_szx = -1;
otCoapOption g_block2_option{OT_COAP_OPTION_BLOCK2, 1};
// OSCORE option (9) of the next request, set with fake_ot::set_request_oscore_option()
uint8_t g_oscore_value[32]{};
otCoapOption g_oscore_option{9, 0};
bool g_has_oscore_option = false;
uint16_t g_matched_option = 0;
otCoapResponseHandler g_request_handler = nullptr;
void *g_request_ctx = nullptr;
int g_requests_sent = 0;
bool g_fail_next_new_message = false;
bool g_fail_next_send = false;
bool g_fail_coap_start = false;
uint32_t g_poll_period = 5000;

}  // namespace

namespace esphome::coap_server::testing::fake_ot {

void reset() {
  g_last_response_len = 0;
  g_last_response_code = 0;
  g_inline_send_called = false;
  g_blockwise_hook = nullptr;
  g_blockwise_ctx = nullptr;
  g_response_has_block2 = false;
  g_response_block2_num = 0;
  g_response_block2_more = false;
  g_response_block2_szx = 0;
  g_request_block2_szx = -1;
  g_has_oscore_option = false;
  g_matched_option = 0;
  g_request_handler = nullptr;
  g_request_ctx = nullptr;
  g_requests_sent = 0;
  g_fail_next_new_message = false;
  g_fail_next_send = false;
  g_fail_coap_start = false;
  g_poll_period = 5000;
}

std::string last_response() {
  return std::string(reinterpret_cast<const char *>(g_last_response), g_last_response_len);
}
int last_response_code() { return g_last_response_code; }
bool inline_send_called() { return g_inline_send_called; }
bool blockwise_called() { return g_blockwise_hook != nullptr; }
void *blockwise_ctx() { return g_blockwise_ctx; }
bool response_has_block2() { return g_response_has_block2; }
uint32_t response_block2_num() { return g_response_block2_num; }
bool response_block2_more() { return g_response_block2_more; }
int response_block2_szx() { return g_response_block2_szx; }
void set_request_block2(int szx) { g_request_block2_szx = szx; }
void set_request_oscore_option(const uint8_t *value, uint8_t len) {
  if (len > sizeof(g_oscore_value))
    len = sizeof(g_oscore_value);
  memcpy(g_oscore_value, value, len);
  g_oscore_option.mLength = len;
  g_has_oscore_option = true;
}
int requests_sent() { return g_requests_sent; }
bool has_pending_request() { return g_request_handler != nullptr; }
void *pending_request_ctx() { return g_request_ctx; }
void complete_pending_request(otError error) {
  otCoapResponseHandler handler = g_request_handler;
  void *ctx = g_request_ctx;
  g_request_handler = nullptr;
  g_request_ctx = nullptr;
  if (handler != nullptr)
    handler(ctx, nullptr, nullptr, error);
}
void fail_next_new_message() { g_fail_next_new_message = true; }
void fail_next_send() { g_fail_next_send = true; }
void fail_coap_start() { g_fail_coap_start = true; }
void set_poll_period(uint32_t ms) { g_poll_period = ms; }

}  // namespace esphome::coap_server::testing::fake_ot

extern "C" {

otError otCoapStart(otInstance * /*instance*/, uint16_t /*port*/) {
  return g_fail_coap_start ? OT_ERROR_FAILED : OT_ERROR_NONE;
}
otError otCoapStop(otInstance * /*instance*/) { return OT_ERROR_NONE; }
void otCoapAddResource(otInstance * /*instance*/, otCoapResource * /*resource*/) {}
void otCoapRemoveResource(otInstance * /*instance*/, otCoapResource * /*resource*/) {}
void otCoapAddBlockWiseResource(otInstance * /*instance*/, otCoapBlockwiseResource * /*resource*/) {}
void otCoapRemoveBlockWiseResource(otInstance * /*instance*/, otCoapBlockwiseResource * /*resource*/) {}

otMessage *otCoapNewMessage(otInstance * /*instance*/, const otMessageInfo * /*info*/) {
  if (g_fail_next_new_message) {
    g_fail_next_new_message = false;
    return nullptr;
  }
  return reinterpret_cast<otMessage *>(alloc_message());
}

void otMessageFree(otMessage * /*msg*/) {}

otError otMessageAppend(otMessage *msg, const void *buf, uint16_t len) {
  FakeMessage *m = as_fake(msg);
  if (m->len + len > sizeof(m->buf))
    return OT_ERROR_NO_BUFS;
  memcpy(m->buf + m->len, buf, len);
  m->len += len;
  return OT_ERROR_NONE;
}

uint16_t otMessageGetLength(const otMessage *msg) { return as_fake(msg)->len; }
uint16_t otMessageGetOffset(const otMessage * /*msg*/) { return 0; }

uint16_t otMessageRead(const otMessage *msg, uint16_t offset, void *buf, uint16_t len) {
  const FakeMessage *m = as_fake(msg);
  if (offset >= m->len)
    return 0;
  uint16_t avail = m->len - offset;
  if (len > avail)
    len = avail;
  memcpy(buf, m->buf + offset, len);
  return len;
}

otCoapCode otCoapMessageGetCode(const otMessage *msg) { return as_fake(msg)->code; }
otCoapType otCoapMessageGetType(const otMessage *msg) { return as_fake(msg)->type; }

#if OPENTHREAD_API_VERSION >= 574
otError otCoapMessageInit(otMessage *msg, otCoapType type, otCoapCode code) {
#else
void otCoapMessageInit(otMessage *msg, otCoapType type, otCoapCode code) {
#endif
  FakeMessage *m = as_fake(msg);
  m->type = type;
  m->code = code;
#if OPENTHREAD_API_VERSION >= 574
  return OT_ERROR_NONE;
#endif
}

otError otCoapMessageInitResponse(otMessage *response, const otMessage *request, otCoapType type, otCoapCode code) {
  FakeMessage *r = as_fake(response);
  const FakeMessage *q = as_fake(request);
  r->type = type;
  r->code = code;
  memcpy(r->token, q->token, q->token_len);
  r->token_len = q->token_len;
  return OT_ERROR_NONE;
}

otError otCoapMessageAppendContentFormatOption(otMessage * /*msg*/, otCoapOptionContentFormat /*fmt*/) {
  return OT_ERROR_NONE;
}

otError otCoapMessageAppendObserveOption(otMessage *msg, uint32_t observe) {
  FakeMessage *m = as_fake(msg);
  m->has_observe = true;
  m->observe_value = observe;
  return OT_ERROR_NONE;
}

otError otCoapMessageAppendOption(otMessage * /*msg*/, uint16_t /*number*/, uint16_t /*len*/, const void * /*value*/) {
  return OT_ERROR_NONE;
}

otError otCoapMessageAppendBlock2Option(otMessage * /*msg*/, uint32_t num, bool more, otCoapBlockSzx szx) {
  g_response_has_block2 = true;
  g_response_block2_num = num;
  g_response_block2_more = more;
  g_response_block2_szx = static_cast<int>(szx);
  return OT_ERROR_NONE;
}

otError otCoapMessageAppendUriPathOptions(otMessage * /*msg*/, const char * /*uri*/) { return OT_ERROR_NONE; }

otError otCoapMessageSetPayloadMarker(otMessage *msg) {
  as_fake(msg)->has_payload_marker = true;
  return OT_ERROR_NONE;
}

bool otCoapAreTokensEqual(const otMessage *a, const otMessage *b) {
  const FakeMessage *ma = as_fake(a);
  const FakeMessage *mb = as_fake(b);
  return ma->token_len == mb->token_len && memcmp(ma->token, mb->token, ma->token_len) == 0;
}

uint8_t otCoapMessageGetTokenLength(const otMessage *msg) { return as_fake(msg)->token_len; }
const uint8_t *otCoapMessageGetToken(const otMessage *msg) { return as_fake(msg)->token; }

otError otCoapMessageSetToken(otMessage *msg, const uint8_t *token, uint8_t token_len) {
  if (token_len > OT_COAP_MAX_TOKEN_LENGTH)
    return OT_ERROR_INVALID_ARGS;
  FakeMessage *m = as_fake(msg);
  memcpy(m->token, token, token_len);
  m->token_len = token_len;
  return OT_ERROR_NONE;
}

#if OPENTHREAD_API_VERSION >= 569
otError otCoapMessageReadToken(const otMessage *msg, otCoapToken *token) {
  const FakeMessage *m = as_fake(msg);
  memcpy(token->m8, m->token, m->token_len);
  token->mLength = m->token_len;
  return OT_ERROR_NONE;
}

otError otCoapMessageWriteToken(otMessage *msg, const otCoapToken *token) {
  return otCoapMessageSetToken(msg, token->m8, token->mLength);
}

bool otCoapMessageAreTokensEqual(const otCoapToken *a, const otCoapToken *b) {
  return a->mLength == b->mLength && memcmp(a->m8, b->m8, a->mLength) == 0;
}
#endif

// Only a Block2 option set with fake_ot::set_request_block2() or an OSCORE option set with
// fake_ot::set_request_oscore_option() is ever found
otError otCoapOptionIteratorInit(otCoapOptionIterator *it, const otMessage * /*msg*/) {
  memset(it, 0, sizeof(*it));
  return OT_ERROR_NONE;
}

const otCoapOption *otCoapOptionIteratorGetFirstOptionMatching(otCoapOptionIterator * /*it*/, uint16_t num) {
  g_matched_option = num;
  if (num == OT_COAP_OPTION_BLOCK2 && g_request_block2_szx >= 0)
    return &g_block2_option;
  if (num == 9 && g_has_oscore_option)
    return &g_oscore_option;
  return nullptr;
}

const otCoapOption *otCoapOptionIteratorGetNextOptionMatching(otCoapOptionIterator * /*it*/, uint16_t /*num*/) {
  return nullptr;
}

otError otCoapOptionIteratorGetOptionUintValue(otCoapOptionIterator * /*it*/, uint64_t *val) {
  // Block2(NUM=0, M=0, SZX)
  *val = g_request_block2_szx >= 0 ? static_cast<uint64_t>(g_request_block2_szx & 0x7) : 0;
  return OT_ERROR_NONE;
}

otError otCoapOptionIteratorGetOptionValue(otCoapOptionIterator * /*it*/, void *val) {
  if (g_matched_option == 9 && g_has_oscore_option)
    memcpy(val, g_oscore_value, g_oscore_option.mLength);
  return OT_ERROR_NONE;
}

otError otCoapSendResponse(otInstance * /*instance*/, otMessage *msg, const otMessageInfo * /*info*/) {
  FakeMessage *m = as_fake(msg);
  g_last_response_len = m->len;
  g_last_response_code = static_cast<int>(m->code);
  if (m->len > 0)
    memcpy(g_last_response, m->buf, m->len);
  g_inline_send_called = true;
  return OT_ERROR_NONE;
}

// Drives the transmit hook in 512-byte blocks and records the whole payload
otError otCoapSendResponseBlockWise(otInstance * /*instance*/, otMessage *msg, const otMessageInfo * /*info*/,
                                    void *ctx, otCoapBlockwiseTransmitHook hook) {
  g_blockwise_hook = hook;
  g_blockwise_ctx = ctx;
  uint16_t assembled = 0;
  uint32_t pos = 0;
  while (assembled < sizeof(g_last_response)) {
    uint16_t block_len = 512;
    if (assembled + block_len > sizeof(g_last_response))
      block_len = static_cast<uint16_t>(sizeof(g_last_response) - assembled);
    bool more = false;
    if (hook(ctx, g_last_response + assembled, pos, &block_len, &more) != OT_ERROR_NONE || block_len == 0)
      break;
    assembled = static_cast<uint16_t>(assembled + block_len);
    pos += block_len;
    if (!more)
      break;
  }
  g_last_response_len = assembled;
  g_last_response_code = static_cast<int>(as_fake(msg)->code);
  return OT_ERROR_NONE;
}

// Keeps the response handler of the last request with one, as OpenThread does until the request is answered
otError otCoapSendRequest(otInstance * /*instance*/, otMessage * /*msg*/, const otMessageInfo * /*info*/,
                          otCoapResponseHandler handler, void *ctx) {
  if (g_fail_next_send) {
    g_fail_next_send = false;
    return OT_ERROR_NO_BUFS;
  }
  g_requests_sent++;
  if (handler != nullptr) {
    g_request_handler = handler;
    g_request_ctx = ctx;
  }
  return OT_ERROR_NONE;
}

const char *otThreadErrorToString(otError error) {
  switch (error) {
    case OT_ERROR_NONE:
      return "None";
    case OT_ERROR_FAILED:
      return "Failed";
    case OT_ERROR_DROP:
      return "Drop";
    case OT_ERROR_NO_BUFS:
      return "NoBufs";
    default:
      return "Unknown";
  }
}

uint32_t otLinkGetPollPeriod(otInstance * /*instance*/) { return g_poll_period; }

bool otIp6IsAddressEqual(const otIp6Address *a, const otIp6Address *b) {
  return memcmp(a->mFields.m8, b->mFields.m8, sizeof(a->mFields.m8)) == 0;
}

void otIp6AddressToString(const otIp6Address *addr, char *buf, uint16_t size) {
  snprintf(buf, size, "%02x%02x::%02x%02x", addr->mFields.m8[0], addr->mFields.m8[1], addr->mFields.m8[14],
           addr->mFields.m8[15]);
}

}  // extern "C"
