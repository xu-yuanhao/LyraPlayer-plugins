#pragma once
#include <stdint.h>
#include "message_contracts.h"
// Registry-local, opaque capability identities, not pointers and not persistent IDs.
// Thread: lookup/enumeration/mutation/execution run on the registry owner
// (creating) thread, which is the host frame thread.
// 0 means absent/invalid. Nonzero handles are never reused in this registry instance.
// A default capability keeps its handle through provide(same name), refresh,
// external replacement and restoration. RuntimeServices caches only these defaults.
// A capability with no default is revoked when its module/publisher unloads;
// replacing that module or publishing the same name again obtains a NEW handle.
// Refresh of unchanged modules preserves handles; refresh that retires a module
// follows the above revocation rule. Enumeration order/index is not stable.
// On stale/0/detached handles: describe/frame/input/supply return -1,
// interface returns NULL; request returns UNSUPPORTED (detached: INVALID).
// describe requires size==sizeof; for a correctly sized output, any failure
// clears flags/handle/string pointers. Invalid-size outputs are untouched.
// describe strings and interface pointers are borrowed until registry mutation;
// do not retain them across refresh/unload. Re-resolve revoked identities by name.
// Context pointers are borrowed while their registry is alive; detached error
// behavior does not permit calls after registry destruction.
// Handles from another instance must never be passed here. No private C++ objects
// cross this ABI.
#define LYRA_INTERFACE_SERVICE_CONTEXT 16u
#define LYRA_SERVICE_SELECTED 0u
#define LYRA_SERVICE_DEFAULT 1u
#define LYRA_SERVICE_OVERRIDDEN 1u
#define LYRA_SERVICE_FRAME 2u
#define LYRA_SERVICE_INPUT 4u
#define LYRA_SERVICE_SUPPLY 16u
#define LYRA_SERVICE_FALLBACK_FRAME 32u
#define LYRA_SERVICE_DEFAULT_ACTIVE 64u
#define LYRA_SERVICE_REQUEST 128u
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
  uint32_t (*count)(void *);
  uint64_t (*at)(void *, uint32_t);
  uint64_t (*publish)(void *, const LyraCapabilityV3 *);
  int (*request)(void *, uint64_t, const LyraMessageV3 *, LyraReplyV3 *);
  int (*describe)(void *, uint64_t, LyraServiceStateV3 *);
  int (*frame)(void *, uint64_t, uint32_t implementation, int64_t,
               const struct LyraBox *);
  int (*input)(void *, uint64_t, uint32_t, const struct LyraInput *);
  int (*hit)(void *, const struct LyraInput *);
  int (*supply)(void *, uint64_t, uint32_t, int64_t, char *, int);
  const void *(*interface)(void *, uint64_t, uint32_t, uint32_t, uint32_t,
                           uint32_t);
} LyraServiceContextV3;
