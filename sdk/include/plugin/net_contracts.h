#pragma once
#include "message_contracts.h"
// First-class host network, loopback-listener and inbound-link services for
// plugins. All calls run on the registry owner (frame) thread; jobs execute on
// host worker threads. request/poll/cancel follow the media-resource pattern:
// a job id is returned, POLL reports state, the body arrives as an owned fd
// that the caller reads and closes. Nothing here is specific to any plugin;
// cache sizes, timeouts and body limits are chosen per request by the caller.
#define LYRA_INTERFACE_NET 32u

enum {
    LYRA_NET_REQUEST = 1,
    LYRA_NET_POLL = 2,
    LYRA_NET_CANCEL = 3,
    LYRA_NET_INBOUND = 4,
    LYRA_NET_LISTEN_OPEN = 5,
    LYRA_NET_LISTEN_POLL = 6,
    LYRA_NET_LISTEN_ANSWER = 7,
    LYRA_NET_LISTEN_CLOSE = 8
};
enum {
    LYRA_NET_JOB = 1,
    LYRA_NET_METHOD = 2,
    LYRA_NET_URL = 3,
    LYRA_NET_HEADERS = 4,
    LYRA_NET_BODY = 5,
    LYRA_NET_MAX_BYTES = 6,
    LYRA_NET_TIMEOUT_MS = 7,
    LYRA_NET_STATE = 8,
    LYRA_NET_STATUS = 9,
    LYRA_NET_FD = 10,
    LYRA_NET_LINKS = 11,
    LYRA_NET_LISTEN_JOB = 12,
    LYRA_NET_PORT = 13,
    LYRA_NET_PREFIX = 14,
    LYRA_NET_REQUEST_ID = 15,
    LYRA_NET_RESPONSE_STATUS = 16,
    LYRA_NET_RESPONSE_HEADERS = 17,
    LYRA_NET_RESPONSE_BODY = 18
};
// POLL STATE values: 0 pending, 1 complete, 2 failed.
// LISTEN_OPEN fields: PORT (i64, 0 picks an ephemeral port), PREFIX (utf8,
// optional path prefix). Reply carries JOB and PORT.
// LISTEN_POLL reply carries REQUEST_ID and FD of a file holding the raw HTTP
// request (request line, headers, body). LISTEN_ANSWER fields: LISTEN_JOB,
// REQUEST_ID, RESPONSE_STATUS (i64), RESPONSE_HEADERS (utf8 JSON object),
// RESPONSE_BODY (bytes, may be empty). The listener closes after ANSWER.
// INBOUND reply carries LINKS: UTF-8, one absolute URI per line, for links
// addressed to the calling plugin (lyra://plugin/<id>/<path...>).
typedef struct LyraNetInterfaceV3 {
    uint32_t size, version;
    void *user;
    int (*request)(void *, const LyraMessageV3 *, LyraReplyV3 *);
} LyraNetInterfaceV3;
