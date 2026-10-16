#pragma once
#include "message_contracts.h"
#define LYRA_INTERFACE_STEM_ANALYSIS 24u
// Synchronous, worker-only endpoint. No audio-thread calls. Returned bytes are
// borrowed until the next request. OPEN_SOURCE returns a caller-owned fd.
typedef struct LyraStemAnalysisV3 {
    uint32_t size,version;
    void *user;
    int (*request)(void *,const LyraMessageV3 *,LyraReplyV3 *);
} LyraStemAnalysisV3;
enum {LYRA_STEM_PROCESS=1,LYRA_STEM_INFO=2,LYRA_STEM_OPEN_SOURCE=3,
      LYRA_STEM_LIBRARY=4,LYRA_STEM_RELEASE=5};
enum {LYRA_STEM_PCM=1,LYRA_STEM_VOCALS=2,LYRA_STEM_SAMPLE_RATE=3,
      LYRA_STEM_FRAME_COUNT=4,LYRA_STEM_HOP_FRAMES=5,LYRA_STEM_CONTEXT_FRAMES=6,
      LYRA_STEM_MODEL=7,LYRA_STEM_TRACK_ID=8,LYRA_STEM_FD=9,
      LYRA_STEM_LIBRARY_LIMIT=10,LYRA_STEM_LIBRARY_ROWS=11};
// Fixed trained-model format, not playback/output sample-rate settings.
enum {LYRA_STEM_RATE=44100,LYRA_STEM_FFT=4096,LYRA_STEM_FFT_HOP=1024,
      LYRA_STEM_SPECTRAL_FRAMES=64,LYRA_STEM_BINS=1024,
      LYRA_STEM_INPUT_FRAMES=68608,LYRA_STEM_OUTPUT_HOP=32768,
      LYRA_STEM_CONTEXT=16384};
