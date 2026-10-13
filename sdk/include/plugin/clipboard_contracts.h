#pragma once
#include "message_contracts.h"
#define LYRA_INTERFACE_CLIPBOARD 19u
enum { LYRA_CLIPBOARD_COPY = 1, LYRA_CLIPBOARD_TEXT = 1, LYRA_CLIPBOARD_LABEL = 2 };
// Frame-thread request. UTF-8 fields are borrowed for this call only. TEXT is
// required, LABEL optional. No field pointers are retained. Unknown required
// fields fail; no older interface layout is accepted.
typedef struct LyraClipboardInterfaceV3 {
    uint32_t size, version;
    void *user;
    int (*request)(void *, const LyraMessageV3 *, LyraReplyV3 *);
} LyraClipboardInterfaceV3;
