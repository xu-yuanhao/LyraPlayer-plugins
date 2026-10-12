#pragma once
#include <stdint.h>
// Optional interfaces: existing ABI 3 structures are unchanged.
#define LYRA_INTERFACE_MEDIA_CONTEXT 13u
#define LYRA_INTERFACE_MEDIA_PROVIDER 14u
#define LYRA_MEDIA_SEARCH 1u
#define LYRA_MEDIA_COLLECTION 2u
#define LYRA_MEDIA_AUDIO 3u
#define LYRA_MEDIA_COVER 4u
#define LYRA_MEDIA_LYRICS 5u
#define LYRA_MEDIA_APPEND 1u
#define LYRA_MEDIA_COMPLETE 2u
#define LYRA_MEDIA_US_PER_MS 1000

typedef struct LyraMediaDescriptorV3 {
    uint32_t size;
    const char *uid, *title, *artist, *album;
    const char *genre, *folder, *audio_uri, *cover_uri, *lyrics_uri, *lyrics_text;
    int64_t duration_us, cover_key;
    // Optional provenance for collections containing items from other providers.
    const char *origin_provider, *origin_uid;
} LyraMediaDescriptorV3;
typedef struct LyraMediaRequestV3 {
    uint32_t size, kind;
    uint64_t request_id, query_revision;
    const char *collection, *query, *cursor, *uid;
} LyraMediaRequestV3;
typedef struct LyraMediaBatchV3 {
    uint32_t size, flags;
    uint64_t request_id;
    const LyraMediaDescriptorV3 *items;
    uint32_t count;
    const char *cursor;
    int32_t error;
} LyraMediaBatchV3;
typedef struct LyraMediaResourceV3 {
    uint32_t size;
    uint64_t request_id;
    int32_t fd, error;
    const char *uri, *headers_json, *mime;
} LyraMediaResourceV3;
// Publishing copies metadata before returning. A resource fd is consumed even
// when a result is rejected as stale. Never publish while shutdown is returning.
typedef struct LyraMediaSinkV3 {
    uint32_t size, version;
    void *user;
    int (*publish)(void *, const LyraMediaBatchV3 *);
    int (*resolved)(void *, const LyraMediaResourceV3 *);
} LyraMediaSinkV3;
typedef struct LyraMediaProviderV3 {
    uint32_t size, version;
    void *user;
    const char *id;
    // Request/cancel are frame-thread calls; publish/resolved can run on workers.
    // Copy request strings before request returns. The sink table remains valid
    // until provider shutdown returns; cancelled request IDs cannot publish.
    int (*request)(void *, const LyraMediaRequestV3 *, const LyraMediaSinkV3 *);
    void (*cancel)(void *, uint64_t);
} LyraMediaProviderV3;
typedef struct LyraMediaCollectionStateV3 {
    uint32_t size;
    int32_t error;
    uint32_t complete;
    const char *cursor;
} LyraMediaCollectionStateV3;
typedef struct LyraMediaContextV3 {
    uint32_t size, version;
    void *user;
    int (*provider_count)(void *);
    int (*provider_id)(void *, int, char *, int);
    uint64_t (*query_revision)(void *);
    int (*query_text)(void *, char *, int);
    // Compare-and-set; rejects an asynchronous response to an older query.
    int (*set_query)(void *, uint64_t, const char *);
    uint64_t (*request)(void *, const char *provider, uint32_t kind,
                        const char *collection, const char *query, const char *cursor, const char *uid);
    void (*cancel)(void *, uint64_t);
    uint64_t (*revision)(void *, uint64_t);
    int (*count)(void *, uint64_t);
    // Output strings are borrowed until the next frame boundary/revision.
    int (*item)(void *, uint64_t, int, LyraMediaDescriptorV3 *);
    int (*select)(void *, uint64_t, int);
    // 1 ready, 0 pending, -1 invalid. Ready transfers fd; copy strings before cancel.
    int (*take_resource)(void *, uint64_t, LyraMediaResourceV3 *);
    int (*current)(void *, LyraMediaDescriptorV3 *);
    int (*queue_count)(void *);
    int (*queue_item)(void *, int, LyraMediaDescriptorV3 *);
    int (*queue_select)(void *, int);
    // Cursor/status strings have the same borrowed lifetime as item strings.
    int (*collection_state)(void *, uint64_t, LyraMediaCollectionStateV3 *);
} LyraMediaContextV3;
#define LYRA_MEDIA_DECIMAL_BASE 10
