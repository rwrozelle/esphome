#pragma once

#include <cstdint>
#include <string>

// What the fake OpenThread CoAP API recorded, for tests of the OpenThread transport
namespace esphome::coap_server::testing::fake_ot {

// Clears everything below and the simulated request Block2 option
void reset();

// Last response sent: the payload (assembled from all blocks for a block-wise send) and its code
std::string last_response();
int last_response_code();
bool inline_send_called();  // otCoapSendResponse
bool blockwise_called();    // otCoapSendResponseBlockWise
void *blockwise_ctx();      // the context passed to otCoapSendResponseBlockWise

// Block2 option appended to the response
bool response_has_block2();
uint32_t response_block2_num();
bool response_block2_more();
int response_block2_szx();

// Makes the next request carry Block2(NUM=0, SZX=szx); -1 for no Block2 option
void set_request_block2(int szx);
// Makes requests carry an OSCORE option (9) with this value, until reset()
void set_request_oscore_option(const uint8_t *value, uint8_t len);

// Requests sent with otCoapSendRequest (notifications, pings)
int requests_sent();
// The last of them: its payload, code, type and token; code and type are -1 after reset()
std::string last_request();
int last_request_code();
int last_request_type();
std::string last_request_token();
// The last request sent with a response handler, until it is completed
bool has_pending_request();
void *pending_request_ctx();
// Calls the pending request's response handler with error, as OpenThread does on an ACK, RST or timeout
void complete_pending_request(otError error);
// Makes the next otCoapNewMessage or otCoapSendRequest fail
void fail_next_new_message();
void fail_next_send();
// Makes otCoapStart fail until reset()
void fail_coap_start();
// What otLinkGetPollPeriod returns; 5000 after reset()
void set_poll_period(uint32_t ms);

}  // namespace esphome::coap_server::testing::fake_ot
