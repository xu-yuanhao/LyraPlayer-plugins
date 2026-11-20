#pragma once
#include "message_contracts.h"
#define LYRA_INTERFACE_MEDIA_ACTIONS 27u
// Frame-thread commands over a live MEDIA_CONTEXT collection. Acceptance copies
// the selected item into the session: cancelling the source request, changing
// search, or unloading the provider cannot revoke the queue entry's metadata.
// Resource resolution still requires its provider; unavailable is not local-file
// fallback. COLLECTION and ROW are required I64 fields; neither is a persistent
// media identity. APPEND creates a new occurrence; NEXT schedules one occurrence
// without replacing the current queue or restarting the current decoder.
enum {LYRA_MEDIA_ACTION_NEXT=1,LYRA_MEDIA_ACTION_APPEND=2};
enum {LYRA_MEDIA_ACTION_COLLECTION=1,LYRA_MEDIA_ACTION_ROW=2};
typedef struct LyraMediaActionsV3 {
 uint32_t size,version;
 void *user;
 int (*request)(void *,const LyraMessageV3 *,LyraReplyV3 *);
} LyraMediaActionsV3;
