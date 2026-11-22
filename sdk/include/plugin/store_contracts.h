#pragma once
#include "message_contracts.h"
// Plugin-scoped key/value storage owned by the host. The calling plugin's
// identity is taken from the registry invocation, never from message fields,
// so one plugin cannot read or write another plugin's keys. SECRET values are
// encrypted at rest with the platform keystore; non-secret values are stored
// as opaque bytes. Keys are UTF-8 without NUL or '\n', at most 256 bytes;
// values at most 1 MiB. KEYS replies with one UTF-8 field of '\n'-separated
// key names (empty when none). GET replies VALUE plus FOUND (i64 0/1).
#define LYRA_INTERFACE_STORE 33u

enum { LYRA_STORE_GET = 1, LYRA_STORE_SET = 2, LYRA_STORE_DELETE = 3, LYRA_STORE_KEYS = 4 };
enum {
    LYRA_STORE_KEY = 1,
    LYRA_STORE_VALUE = 2,
    LYRA_STORE_SECRET = 3,
    LYRA_STORE_KEYS_OUT = 4,
    LYRA_STORE_FOUND = 5
};
typedef struct LyraStoreInterfaceV3 {
    uint32_t size, version;
    void *user;
    int (*request)(void *, const LyraMessageV3 *, LyraReplyV3 *);
} LyraStoreInterfaceV3;
