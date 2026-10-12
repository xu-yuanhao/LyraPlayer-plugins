#pragma once
#include <stdint.h>
// Optional, instance-scoped service execution. No private C++ objects cross
// ABI.
#define LYRA_INTERFACE_SERVICE_CONTEXT 16u
#define LYRA_SERVICE_SELECTED 0u
#define LYRA_SERVICE_DEFAULT 1u
#define LYRA_SERVICE_OVERRIDDEN 1u
#define LYRA_SERVICE_FRAME 2u
#define LYRA_SERVICE_INPUT 4u
#define LYRA_SERVICE_SUPPLY 16u
#define LYRA_SERVICE_FALLBACK_FRAME 32u
#define LYRA_SERVICE_DEFAULT_ACTIVE 64u
struct LyraInput;
struct LyraBox;
typedef struct LyraServiceStateV3 {
  uint32_t size, flags;
  uint64_t handle;
  const char *capability, *selected_id, *default_id;
} LyraServiceStateV3;
typedef struct LyraServiceContextV3 {
  uint32_t size, version;
  void *user;
  uint64_t (*lookup)(void *, const char *);
  uint64_t (*current)(void *);
  int (*describe)(void *, uint64_t, LyraServiceStateV3 *);
  int (*frame)(void *, uint64_t, uint32_t implementation, int64_t,
               const struct LyraBox *);
  int (*input)(void *, uint64_t, uint32_t, const struct LyraInput *);
  int (*hit)(void *, const struct LyraInput *);
  int (*supply)(void *, uint64_t, uint32_t, int64_t, char *, int);
  const void *(*interface)(void *, uint64_t, uint32_t, uint32_t, uint32_t,
                           uint32_t);
} LyraServiceContextV3;
