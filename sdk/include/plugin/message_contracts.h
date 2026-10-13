#pragma once
#include <stdint.h>
// One current protocol. Optional fields extend messages, not C layouts.
#define LYRA_FIELD_REQUIRED 1u
#define LYRA_FIELD_I64 1u
#define LYRA_FIELD_F64 2u
#define LYRA_FIELD_UTF8 3u
#define LYRA_FIELD_BYTES 4u
#define LYRA_FIELD_HANDLE 5u
#define LYRA_MESSAGE_OK 0
#define LYRA_MESSAGE_INVALID -1
#define LYRA_MESSAGE_UNSUPPORTED -2
#define LYRA_MESSAGE_CAPACITY -3

typedef struct LyraFieldV3 {
    uint32_t tag, type, flags, bytes;
    const void *data;
} LyraFieldV3;
typedef struct LyraMessageV3 {
    uint32_t size, field_count;
    uint64_t operation;
    const LyraFieldV3 *fields;
} LyraMessageV3;
// Caller owns field descriptors; returned data is borrowed until the next call.
typedef struct LyraReplyV3 {
    uint32_t size, capacity, field_count;
    int32_t status;
    LyraFieldV3 *fields;
} LyraReplyV3;
typedef struct LyraCapabilityV3 {
    uint32_t size;
    const char *name;
    void *user;
    int (*request)(void *, const LyraMessageV3 *, LyraReplyV3 *);
} LyraCapabilityV3;
// Endpoint-owned schemas validate known fields. Unknown optional fields are
// ignored; unknown required fields fail. No older wire layout is accepted.
typedef struct LyraFieldRuleV3 { uint32_t tag, type; } LyraFieldRuleV3;
static inline int lyra_message_fields(const LyraMessageV3 *message,
                                      const LyraFieldRuleV3 *rules, uint32_t count) {
    if (!message || message->size != sizeof(*message) ||
        (message->field_count && !message->fields) || (count && !rules)) return LYRA_MESSAGE_INVALID;
    for (uint32_t i = 0; i < message->field_count; ++i) {
        const LyraFieldV3 *field = &message->fields[i];
        uint32_t r = 0;
        for (; r < count && rules[r].tag != field->tag; ++r) {}
        if (r == count) { if (field->flags & LYRA_FIELD_REQUIRED) return LYRA_MESSAGE_UNSUPPORTED; }
        else if (rules[r].type != field->type) return LYRA_MESSAGE_INVALID;
    }
    return LYRA_MESSAGE_OK;
}
