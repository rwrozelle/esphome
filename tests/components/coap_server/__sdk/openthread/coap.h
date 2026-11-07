#pragma once
// Stand-in for OpenThread's CoAP API, limited to what the coap_server OpenThread transport uses.
// Implemented in fake_openthread.cpp.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "openthread/instance.h"
#include "openthread/ip6.h"

// ---------------------------------------------------------------------------
// Basic OT types
// ---------------------------------------------------------------------------

// otInstance must be a complete type so InstanceLock::get_instance() can instantiate it.
struct otInstance {};

typedef int otError;
static constexpr otError OT_ERROR_NONE = 0;
static constexpr otError OT_ERROR_FAILED = 1;
static constexpr otError OT_ERROR_DROP = 2;
static constexpr otError OT_ERROR_NO_BUFS = 3;
static constexpr otError OT_ERROR_NOT_FOUND = 5;
static constexpr otError OT_ERROR_PARSE = 6;
static constexpr otError OT_ERROR_INVALID_ARGS = 7;
static constexpr otError OT_ERROR_RESPONSE_TIMEOUT = 26;

// ---------------------------------------------------------------------------
// CoAP message types and codes
// ---------------------------------------------------------------------------

typedef enum {
  OT_COAP_TYPE_CONFIRMABLE = 0,
  OT_COAP_TYPE_NON_CONFIRMABLE = 1,
  OT_COAP_TYPE_ACKNOWLEDGMENT = 2,
  OT_COAP_TYPE_RESET = 3,
} otCoapType;

typedef enum {
  OT_COAP_CODE_GET = 1,
  OT_COAP_CODE_POST = 2,
  OT_COAP_CODE_PUT = 3,
  OT_COAP_CODE_DELETE = 4,
  OT_COAP_CODE_EMPTY = 0,
  OT_COAP_CODE_CREATED = 65,
  OT_COAP_CODE_DELETED = 66,
  OT_COAP_CODE_VALID = 67,
  OT_COAP_CODE_CHANGED = 68,
  OT_COAP_CODE_CONTENT = 69,
  OT_COAP_CODE_CONTINUE = 95,
  OT_COAP_CODE_BAD_REQUEST = 128,
  OT_COAP_CODE_UNAUTHORIZED = 129,
  OT_COAP_CODE_BAD_OPTION = 130,
  OT_COAP_CODE_FORBIDDEN = 131,
  OT_COAP_CODE_NOT_FOUND = 132,
  OT_COAP_CODE_METHOD_NOT_ALLOWED = 133,
  OT_COAP_CODE_CHANGED_CONTENT_FORMAT = 134,
  OT_COAP_CODE_INTERNAL_ERROR = 160,
} otCoapCode;

typedef enum {
  OT_COAP_OPTION_CONTENT_FORMAT_LINK_FORMAT = 40,
  OT_COAP_OPTION_CONTENT_FORMAT_CBOR = 60,
  OT_COAP_OPTION_CONTENT_FORMAT_SENML_CBOR = 112,
} otCoapOptionContentFormat;

typedef enum {
  OT_COAP_OPTION_OBSERVE = 6,
  OT_COAP_OPTION_CONTENT_FORMAT = 12,
  OT_COAP_OPTION_BLOCK2 = 23,
} otCoapOptionNumber;

// Block size exponent for RFC 7959 block-wise transfer
typedef enum {
  OT_COAP_BLOCK_SZX_16 = 0,
  OT_COAP_BLOCK_SZX_32 = 1,
  OT_COAP_BLOCK_SZX_64 = 2,
  OT_COAP_BLOCK_SZX_128 = 3,
  OT_COAP_BLOCK_SZX_256 = 4,
  OT_COAP_BLOCK_SZX_512 = 5,
  OT_COAP_BLOCK_SZX_1024 = 6,
} otCoapBlockSzx;

// ---------------------------------------------------------------------------
// CoAP token
// ---------------------------------------------------------------------------

static constexpr uint8_t OT_COAP_MAX_TOKEN_LENGTH = 8;

// Added in API 569; before that ot_coap_compat.h provides it
#if OPENTHREAD_API_VERSION >= 569
typedef struct otCoapToken {
  uint8_t m8[OT_COAP_MAX_TOKEN_LENGTH];
  uint8_t mLength;
} otCoapToken;
#endif

// ---------------------------------------------------------------------------
// CoAP option iterator
// ---------------------------------------------------------------------------

typedef struct {
  uint32_t mOption;
  uint16_t mLength;
} otCoapOption;

typedef struct {
  const void *mMessage;
  uint16_t mOffset;
  otCoapOption mOption;
  bool mFirstOptionDone;
} otCoapOptionIterator;

// ---------------------------------------------------------------------------
// CoAP resource and message
// ---------------------------------------------------------------------------

typedef struct otMessage otMessage;

typedef struct {
  otIp6Address mPeerAddr;
  uint16_t mPeerPort;
  otIp6Address mSockAddr;
  uint16_t mSockPort;
} otMessageInfo;

typedef void (*otCoapRequestHandler)(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo);
typedef void (*otCoapResponseHandler)(void *aContext, otMessage *aMessage, const otMessageInfo *aMessageInfo,
                                      otError aError);

typedef struct otCoapResource {
  const char *mUriPath;
  otCoapRequestHandler mHandler;
  void *mContext;
  struct otCoapResource *mNext;
} otCoapResource;

// Block-wise transfer hooks (RFC 7959 / OPENTHREAD_CONFIG_COAP_BLOCKWISE_TRANSFER_ENABLE)
typedef otError (*otCoapBlockwiseTransmitHook)(void *aContext, uint8_t *aBlock, uint32_t aPosition,
                                               uint16_t *aBlockLength, bool *aMore);
typedef otError (*otCoapBlockwiseReceiveHook)(void *aContext, uint8_t *aBlock, uint32_t aPosition,
                                              uint16_t aBlockLength, bool aMore, uint32_t aTotalLength);

typedef struct otCoapBlockwiseResource {
  const char *mUriPath;
  otCoapRequestHandler mHandler;
  otCoapBlockwiseReceiveHook mReceiveHook;
  otCoapBlockwiseTransmitHook mTransmitHook;
  void *mContext;
  struct otCoapBlockwiseResource *mNext;
} otCoapBlockwiseResource;

// ---------------------------------------------------------------------------
// C function declarations — implemented in fake_openthread.cpp
// ---------------------------------------------------------------------------

#ifdef __cplusplus
extern "C" {
#endif

otError otCoapStart(otInstance *aInstance, uint16_t aPort);
otError otCoapStop(otInstance *aInstance);
void otCoapAddResource(otInstance *aInstance, otCoapResource *aResource);
void otCoapRemoveResource(otInstance *aInstance, otCoapResource *aResource);

otMessage *otCoapNewMessage(otInstance *aInstance, const otMessageInfo *aMessageInfo);
void otMessageFree(otMessage *aMessage);
otError otMessageAppend(otMessage *aMessage, const void *aBuf, uint16_t aLength);
uint16_t otMessageGetLength(const otMessage *aMessage);
uint16_t otMessageGetOffset(const otMessage *aMessage);
uint16_t otMessageRead(const otMessage *aMessage, uint16_t aOffset, void *aBuf, uint16_t aLength);

otCoapCode otCoapMessageGetCode(const otMessage *aMessage);
otCoapType otCoapMessageGetType(const otMessage *aMessage);

#if OPENTHREAD_API_VERSION >= 574
otError otCoapMessageInit(otMessage *aMessage, otCoapType aType, otCoapCode aCode);
#else
void otCoapMessageInit(otMessage *aMessage, otCoapType aType, otCoapCode aCode);
#endif
otError otCoapMessageInitResponse(otMessage *aResponse, const otMessage *aRequest, otCoapType aType, otCoapCode aCode);
otError otCoapMessageAppendContentFormatOption(otMessage *aMessage, otCoapOptionContentFormat aContentFormat);
otError otCoapMessageAppendObserveOption(otMessage *aMessage, uint32_t aObserve);
otError otCoapMessageAppendOption(otMessage *aMessage, uint16_t aNumber, uint16_t aLength, const void *aValue);
otError otCoapMessageAppendUriPathOptions(otMessage *aMessage, const char *aUriPath);
otError otCoapMessageSetPayloadMarker(otMessage *aMessage);

bool otCoapAreTokensEqual(const otMessage *aFirst, const otMessage *aSecond);
uint8_t otCoapMessageGetTokenLength(const otMessage *aMessage);
const uint8_t *otCoapMessageGetToken(const otMessage *aMessage);
otError otCoapMessageSetToken(otMessage *aMessage, const uint8_t *aToken, uint8_t aTokenLength);

#if OPENTHREAD_API_VERSION >= 569
otError otCoapMessageReadToken(const otMessage *aMessage, otCoapToken *aToken);
otError otCoapMessageWriteToken(otMessage *aMessage, const otCoapToken *aToken);
bool otCoapMessageAreTokensEqual(const otCoapToken *aFirstToken, const otCoapToken *aSecondToken);
#endif

otError otCoapOptionIteratorInit(otCoapOptionIterator *aIterator, const otMessage *aMessage);
const otCoapOption *otCoapOptionIteratorGetFirstOptionMatching(otCoapOptionIterator *aIterator, uint16_t aNumber);
const otCoapOption *otCoapOptionIteratorGetNextOptionMatching(otCoapOptionIterator *aIterator, uint16_t aNumber);
otError otCoapOptionIteratorGetOptionUintValue(otCoapOptionIterator *aIterator, uint64_t *aValue);
otError otCoapOptionIteratorGetOptionValue(otCoapOptionIterator *aIterator, void *aValue);

otError otCoapMessageAppendBlock2Option(otMessage *aMessage, uint32_t aNum, bool aMore, otCoapBlockSzx aSize);

void otCoapAddBlockWiseResource(otInstance *aInstance, otCoapBlockwiseResource *aResource);
void otCoapRemoveBlockWiseResource(otInstance *aInstance, otCoapBlockwiseResource *aResource);

otError otCoapSendResponse(otInstance *aInstance, otMessage *aMessage, const otMessageInfo *aMessageInfo);
otError otCoapSendResponseBlockWise(otInstance *aInstance, otMessage *aMessage, const otMessageInfo *aMessageInfo,
                                    void *aContext, otCoapBlockwiseTransmitHook aTransmitHook);
otError otCoapSendRequest(otInstance *aInstance, otMessage *aMessage, const otMessageInfo *aMessageInfo,
                          otCoapResponseHandler aHandler, void *aContext);

const char *otThreadErrorToString(otError aError);
uint32_t otLinkGetPollPeriod(otInstance *aInstance);

#ifdef __cplusplus
}
#endif
