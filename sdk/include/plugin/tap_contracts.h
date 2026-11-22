#pragma once
#include "message_contracts.h"
// Optional message endpoint a service may expose to observe and intercept
// every message-transport service request. The registry collects tap providers
// from all selected services after each mutation and runs them, in registration
// order, inside Impl::request before the target is invoked. This is the
// generic hook point: it is not keyed to any capability name, so it covers
// settings pages, personal library, listening history, media actions,
// navigation, clipboard/links and every future message endpoint.
//
// The tap receives a normal LyraMessageV3 whose operation and fields are the
// intercepted call's, plus two trailing meta fields:
//   LYRA_TAP_META_HANDLE   (i64) the target service handle
//   LYRA_TAP_META_INTERFACE(i64) the requested interface id (0 for the
//                          capability's own request endpoint)
// Meta tags live above 0xF0000000 and are stripped before dispatch.
//
// The tap answers through the ordinary reply:
//   return LYRA_MESSAGE_UNSUPPORTED or PASS with an empty reply: no opinion,
//       continue to the next tap and finally the target.
//   return LYRA_TAP_REWRITE: reply->fields carries the replacement field set
//       for the intercepted call (without meta fields). An optional meta field
//       LYRA_TAP_META_OPERATION (i64) overrides the operation. The rewritten
//       call is what later taps and the target see.
//   return LYRA_TAP_ANSWER: reply is returned to the original caller after the
//       usual reply validation (status must be a LYRA_MESSAGE_* value).
// Any other return value is logged and treated as PASS. Taps run on the frame
// thread inside the caller's invocation; a tap may itself call the service
// context, bounded by a nesting depth limit.
#define LYRA_INTERFACE_SERVICE_TAP 34u
enum {
    LYRA_TAP_PASS = 0,
    LYRA_TAP_REWRITE = 1,
    LYRA_TAP_ANSWER = 2,
    LYRA_TAP_META_HANDLE = 0xF0000001u,
    LYRA_TAP_META_INTERFACE = 0xF0000002u,
    LYRA_TAP_META_OPERATION = 0xF0000003u
};
typedef struct LyraServiceTapV3 {
    uint32_t size, version;
    void *user;
    int (*request)(void *, const LyraMessageV3 *, LyraReplyV3 *);
} LyraServiceTapV3;
