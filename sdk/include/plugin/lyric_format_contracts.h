#pragma once
#include "message_contracts.h"

#define LYRA_INTERFACE_LYRIC_FORMAT 30u

// Optional on the selected lyric source. NORMALIZE is synchronous on the
// registry owner thread. INPUT is raw bytes, OUTPUT is canonical lyric text
// (UTF-8 bytes without a terminator). An empty OUTPUT replaces the lyric with
// an empty document. UNSUPPORTED leaves the input for the standard parser.
enum { LYRA_LYRIC_NORMALIZE = 1 };
enum { LYRA_LYRIC_INPUT = 1, LYRA_LYRIC_OUTPUT = 2 };
typedef struct LyraLyricFormatV3 {
    uint32_t size, version;
    void *user;
    int (*request)(void *, const LyraMessageV3 *, LyraReplyV3 *);
} LyraLyricFormatV3;
