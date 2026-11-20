#pragma once
#include "message_contracts.h"
#include "contracts.h"
#define LYRA_INTERFACE_LISTENING_HISTORY 22u
// QUERY takes no fields. Reply is a revision HANDLE plus UTF-8 JSON:
// {year, weekday (Sunday=0), days: [milliseconds since Jan 1]}.
// Data is borrowed until the next frame-thread request. No listening titles,
// paths are stored. Optional recent: unique track IDs as decimal strings, newest first.
// Missing history is an empty year and empty recent list.
enum { LYRA_LISTENING_QUERY = 1, LYRA_SLEEP_TIMER = 2 };
// SLEEP_TIMER: optional DURATION (I64 seconds, 0 cancels); no fields queries.
// Reply: REMAINING (I64 seconds, rounded up). Frame-thread requests only.
enum { LYRA_SLEEP_DURATION = 3, LYRA_SLEEP_REMAINING = 4 };
// Bounded wire/storage budget, independent of catalog size.
enum { LYRA_LISTENING_MAX_RECENT = 256 };
enum { LYRA_LISTENING_REVISION = 1, LYRA_LISTENING_SNAPSHOT = 2 };
typedef struct LyraListeningHistoryV3 {
    uint32_t size, version;
    void *user;
    int (*request)(void *, const LyraMessageV3 *, LyraReplyV3 *);
} LyraListeningHistoryV3;
