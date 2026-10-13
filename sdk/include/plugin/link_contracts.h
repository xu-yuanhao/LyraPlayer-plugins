#pragma once
#include "message_contracts.h"
#define LYRA_INTERFACE_LINKS 20u
enum { LYRA_LINK_OPEN_WEB = 1, LYRA_LINK_URI = 1 };
// Frame-thread web navigation. URI is required UTF-8, borrowed for this call.
// Opens an http(s) URI through the system resolver. No intent extras or component
// names are accepted; unsupported platforms return UNSUPPORTED.
typedef struct LyraLinksInterfaceV3 {
    uint32_t size, version;
    void *user;
    int (*request)(void *, const LyraMessageV3 *, LyraReplyV3 *);
} LyraLinksInterfaceV3;
