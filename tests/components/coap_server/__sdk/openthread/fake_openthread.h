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

// Requests sent with otCoapSendRequest (notifications, pings)
int requests_sent();
// The last request sent with a response handler, until it is completed
bool has_pending_request();
void *pending_request_ctx();
// Calls the pending request's response handler with error, as OpenThread does on an ACK, RST or timeout
void complete_pending_request(otError error);
// Makes the next otCoapNewMessage or otCoapSendRequest fail
void fail_next_new_message();
void fail_next_send();

}  // namespace esphome::coap_server::testing::fake_ot
