#pragma once
#include <stdint.h>
// Interface IDs are protocol constants, independent of service registration names.
#define LYRA_INTERFACE_PLAYBACK 1u
#define LYRA_INTERFACE_LYRICS 2u
#define LYRA_INTERFACE_STYLES 3u
#define LYRA_INTERFACE_CONFIG 4u
#define LYRA_INTERFACE_RULES 5u
#define LYRA_INTERFACE_PLAYBACK_PROVIDER 6u
#define LYRA_INTERFACE_CATALOG 7u
#define LYRA_INTERFACE_MODES 8u
#define LYRA_INTERFACE_CONFIG_SCHEMA 9u
#define LYRA_INTERFACE_VERSION 2u
#define LYRA_PLAYBACK_ACTIVE 1u
#define LYRA_PLAYBACK_PAUSED 2u
#define LYRA_PLAYBACK_CLOCK_READY 4u
#define LYRA_PLAYBACK_SEEKING 8u
#define LYRA_PLAYBACK_FEATURES_READY 16u

typedef struct LyraPlaybackSnapshotV2 {
    uint32_t size, flags;
    int64_t track_id, position_us, duration_us, sampled_ns, output_frame;
    uint64_t generation, revision, feature_epoch, seek_serial, landed_seek;
    int32_t sample_rate;
    float energy, bass, mid, high, accent;
} LyraPlaybackSnapshotV2;
enum LyraPlaybackCommandKind {
    LYRA_PLAYBACK_OPEN,
    LYRA_PLAYBACK_STOP,
    LYRA_PLAYBACK_PAUSE,
    LYRA_PLAYBACK_RESUME,
    LYRA_PLAYBACK_SEEK,
    LYRA_PLAYBACK_PRIME,
    LYRA_PLAYBACK_CLEAR_PRIME,
    LYRA_PLAYBACK_REAP,
    LYRA_PLAYBACK_TOGGLE,
    LYRA_PLAYBACK_NEXT,
    LYRA_PLAYBACK_PREVIOUS,
    LYRA_PLAYBACK_SELECT,
    LYRA_PLAYBACK_QUEUE_SELECT,
    LYRA_PLAYBACK_MODE,
    LYRA_PLAYBACK_REMEMBER
};
typedef struct LyraPlaybackCommandV2 {
    uint32_t size, kind, flags, seek_origin;
    int64_t track_id, position_us, duration_us;
    uint64_t generation;
    int32_t fd, index, capacity_ms, bursts;
    float bass_hz, wide_hz, hop_ms, accent_gain;
} LyraPlaybackCommandV2;
// event 0 EOF, 1 failure, 2 actual audio progress, 3 seek failure.
// The optional snapshot is copied during the call and is never retained.
typedef void (*LyraPlaybackEventFn)(void *, int32_t, uint64_t, const LyraPlaybackSnapshotV2 *);
// snapshot writes a value; no returned pointer may refer to a provider's worker data.
// OPEN transfers fd ownership. STOP is followed by quiesce before replacement.
// quiesce returns 1 only after every provider callback/worker has exited.
typedef struct LyraPlaybackProviderV2 {
    uint32_t size, version;
    void *user;
    int (*snapshot)(void *, int64_t, int32_t, LyraPlaybackSnapshotV2 *);
    int (*command)(void *, const LyraPlaybackCommandV2 *);
    int (*quiesce)(void *);
    void (*listener)(void *, LyraPlaybackEventFn, void *);
    int (*lyrics)(void *, uint64_t, int64_t, char *, int);
} LyraPlaybackProviderV2;
typedef struct LyraPlaybackInterfaceV2 {
    uint32_t size, version;
    void *user;
    int (*snapshot)(void *, LyraPlaybackSnapshotV2 *);
    int (*command)(void *, const LyraPlaybackCommandV2 *);
    int (*queue)(void *, int32_t *, int32_t, int32_t *);
} LyraPlaybackInterfaceV2;
typedef struct LyraLyricLineV2 {
    uint32_t size, flags;
    int32_t index, word_count, backing_count;
    int64_t start_us, end_us;
} LyraLyricLineV2;
typedef struct LyraLyricWordV2 {
    uint32_t size;
    int32_t index, units;
    int64_t start_us, end_us;
} LyraLyricWordV2;
typedef struct LyraLyricsInterfaceV2 {
    uint32_t size, version;
    void *user;
    uint64_t (*revision)(void *);
    int64_t (*track_id)(void *);
    int (*count)(void *);
    int (*line)(void *, int32_t, LyraLyricLineV2 *);
    // kind: 0 main, 1 translation, 2 phonetic, 3 backing, 4 word text,
    // 5 word phonetic; part selects backing/word. Return required bytes;
    // a too-small buffer is left untouched. All text is UTF-8.
    int (*text)(void *, int32_t, int32_t, int32_t, char *, int32_t);
    int (*word)(void *, int32_t, int32_t, LyraLyricWordV2 *);
} LyraLyricsInterfaceV2;
struct LyraHostV2;
struct LyraInput;
typedef struct LyraStyleDescriptionV2 {
    uint32_t size;
    char id[64], label[64];
    int32_t background_variant;
    void *user;
    int (*paint)(void *, const struct LyraHostV2 *, int64_t, float, float, float, float);
    int (*input)(void *, const struct LyraHostV2 *, const struct LyraInput *);
} LyraStyleDescriptionV2;
typedef struct LyraStylesInterfaceV2 {
    uint32_t size, version;
    void *user;
    int (*count)(void *);
    int (*describe)(void *, int32_t, LyraStyleDescriptionV2 *);
} LyraStylesInterfaceV2;
typedef struct LyraConfigDescriptionV2 {
    uint32_t size, kind;
    char key[128], value[256], default_value[256];
} LyraConfigDescriptionV2;
typedef struct LyraConfigInterfaceV2 {
    uint32_t size, version;
    void *user;
    int (*count)(void *);
    int (*describe)(void *, int32_t, LyraConfigDescriptionV2 *);
    // The value is a TOML scalar. Validation and activation happen at a frame boundary.
    int (*write)(void *, const char *, const char *);
} LyraConfigInterfaceV2;
typedef struct LyraPlaybackRuleRequestV2 {
    uint32_t size;
    int32_t mode, count, current, delta, automatic;
    int64_t entropy;
} LyraPlaybackRuleRequestV2;
typedef struct LyraPlaybackRuleResultV2 {
    uint32_t size;
    int32_t row, remember;
} LyraPlaybackRuleResultV2;
struct LyraModesInterfaceV2;
typedef struct LyraPlaybackRulesV2 {
    uint32_t size, version;
    void *user;
    int (*next)(void *, const LyraPlaybackRuleRequestV2 *, LyraPlaybackRuleResultV2 *);
    int (*order)(void *, const LyraPlaybackRuleRequestV2 *, int32_t *, int32_t, int32_t *);
    const struct LyraModesInterfaceV2 *modes; // Optional size-checked extension; IDs >= 4.
} LyraPlaybackRulesV2;

// Catalog rows are revision-scoped; text is UTF-8 and never truncated.
typedef struct LyraMediaItemV2 {
    uint32_t size;
    int64_t id, album_id, cover_key, duration_us;
} LyraMediaItemV2;
typedef struct LyraCatalogInterfaceV2 {
    uint32_t size, version;
    void *user;
    uint64_t (*revision)(void *);
    int (*count)(void *);
    int (*item)(void *, uint64_t, int32_t, LyraMediaItemV2 *);
    // field 0 title, 1 artist, 2 album. Returns required bytes including NUL.
    // A stale revision or invalid field returns -1; insufficient buffers are untouched.
    int (*text)(void *, uint64_t, int32_t, int32_t, char *, int32_t);
    // Optional filtered row indices. Writes at most capacity; returns written count.
    int (*query)(void *, uint64_t, const char *, int32_t *, int32_t);
} LyraCatalogInterfaceV2;
typedef struct LyraModeDescriptionV2 {
    uint32_t size;
    int32_t id;
    char label[128];
} LyraModeDescriptionV2;
typedef struct LyraModesInterfaceV2 {
    uint32_t size, version;
    void *user;
    int (*count)(void *);
    int (*describe)(void *, int32_t, LyraModeDescriptionV2 *);
} LyraModesInterfaceV2;

#define LYRA_CONFIG_MINIMUM 1u
#define LYRA_CONFIG_MAXIMUM 2u
#define LYRA_CONFIG_INTEGER 4u
// Strings returned by declarations are borrowed only for the describe call.
typedef struct LyraParameterV2 {
    uint32_t size, kind, flags, schema_version;
    const char *key, *name, *description, *unit, *group, *default_value, *previous_key;
    double minimum, maximum;
} LyraParameterV2;
typedef struct LyraConfigSchemaV2 {
    uint32_t size, version;
    void *user;
    int (*count)(void *);
    int (*describe)(void *, int32_t, LyraParameterV2 *);
    // field 0 value, 1 default, 2 name, 3 description, 4 unit, 5 group, 6 previous key.
    // Host interface only: required UTF-8 bytes including NUL, no partial writes.
    int (*text)(void *, const char *, int32_t, char *, int32_t);
} LyraConfigSchemaV2;

#define LYRA_INTERFACE_PLUGIN_CATALOG 10u
// state: 0 available, 1 installed/current, 2 downloading, 3 installing, 4 update available.
typedef struct LyraPluginEntryV2 {
    uint32_t size, abi, state;
    uint64_t bytes;
    char id[64], title[128], version[32], description[256], capability[24];
} LyraPluginEntryV2;
// Entry indices belong to the current revision. install transfers the validated
// archive to the same package installer; source implementations own networking.
typedef struct LyraPluginCatalogV2 {
    uint32_t size, version;
    void *user;
    uint64_t (*revision)(void *);
    int (*count)(void *);
    int (*describe)(void *, int32_t, LyraPluginEntryV2 *);
    int (*search)(void *, const char *, int32_t *, int32_t);
    int (*install)(void *, int32_t);
    void (*refresh)(void *);
    int (*score)(void *,const char*,int32_t);
} LyraPluginCatalogV2;

// Optional extension: keeps the original catalog ABI/layout unchanged.
#define LYRA_INTERFACE_PLUGIN_DETAILS 11u
// UTF-8 text fields; text() returns required bytes including NUL, never
// truncates. action: 0 install/update, 1 uninstall. installed identifies local
// metadata.
typedef struct LyraPluginDetailsV2 {
  uint32_t size, version;
  void *user;
  int (*text)(void *, const char *id, const char *field, int installed, char *,
              int);
  int (*action)(void *, const char *id, int action);
  int64_t (*icon_key)(void *, const char *id, int installed);
} LyraPluginDetailsV2;
