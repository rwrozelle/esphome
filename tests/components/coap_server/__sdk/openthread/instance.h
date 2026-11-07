#pragma once
// Stand-in for OpenThread's instance.h, limited to the API version. Build the tests with
// COAP_SERVER_OT_API=440 to test ot_coap_compat.h, as NCS ships that version.

#ifndef OPENTHREAD_API_VERSION
#define OPENTHREAD_API_VERSION (621)
#endif
