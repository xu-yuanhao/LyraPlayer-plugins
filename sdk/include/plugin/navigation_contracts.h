#pragma once
#include "message_contracts.h"

#define LYRA_INTERFACE_NAVIGATION 29u

// Optional interface on the selected frame service. BACK_AVAILABLE returns OK
// while the root can consume Back. BACK performs the action. Both use no fields.
enum { LYRA_NAVIGATION_BACK_AVAILABLE = 1, LYRA_NAVIGATION_BACK = 2 };
typedef struct LyraNavigationV3 {
    uint32_t size, version;
    void *user;
    int (*request)(void *, const LyraMessageV3 *, LyraReplyV3 *);
} LyraNavigationV3;
