#pragma once

#include "esphome/core/defines.h"

#ifdef USE_COAP_SERVER_OPENTHREAD

#include <cstring>
#include <openthread/coap.h>
#include <openthread/instance.h>

// NCS ships OpenThread API 440. The CoAP token type and its calls came in API 569, so they are
// provided here from the older calls; they have the same names and behaviour as the newer API.
#if OPENTHREAD_API_VERSION < 569
typedef struct otCoapToken {
  uint8_t m8[OT_COAP_MAX_TOKEN_LENGTH];
  uint8_t mLength;
} otCoapToken;

inline otError otCoapMessageReadToken(const otMessage *aMessage, otCoapToken *aToken) {
  const uint8_t length = otCoapMessageGetTokenLength(aMessage);
  if (length > OT_COAP_MAX_TOKEN_LENGTH)
    return OT_ERROR_PARSE;
  memcpy(aToken->m8, otCoapMessageGetToken(aMessage), length);
  aToken->mLength = length;
  return OT_ERROR_NONE;
}

inline otError otCoapMessageWriteToken(otMessage *aMessage, const otCoapToken *aToken) {
  return otCoapMessageSetToken(aMessage, aToken->m8, aToken->mLength);
}

inline bool otCoapMessageAreTokensEqual(const otCoapToken *aFirstToken, const otCoapToken *aSecondToken) {
  return aFirstToken->mLength == aSecondToken->mLength &&
         memcmp(aFirstToken->m8, aSecondToken->m8, aFirstToken->mLength) == 0;
}
#endif  // OPENTHREAD_API_VERSION < 569

namespace esphome::coap_server {

// otCoapMessageInit returns otError from API 574, and nothing before it
inline otError coap_message_init(otMessage *message, otCoapType type, otCoapCode code) {
#if OPENTHREAD_API_VERSION < 574
  otCoapMessageInit(message, type, code);
  return OT_ERROR_NONE;
#else
  return otCoapMessageInit(message, type, code);
#endif
}

}  // namespace esphome::coap_server

#endif  // USE_COAP_SERVER_OPENTHREAD
