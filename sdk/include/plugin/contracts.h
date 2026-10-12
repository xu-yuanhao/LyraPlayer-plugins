#pragma once
#include <stdint.h>
// Interface IDs are protocol constants, independent of service registration names.
#define LYRA_INTERFACE_PLAYBACK 1u
#define LYRA_INTERFACE_LYRICS 2u
#define LYRA_INTERFACE_STYLES 3u
#define LYRA_INTERFACE_CONFIG 4u
#define LYRA_INTERFACE_RULES 5u
#define LYRA_INTERFACE_PLAYBACK_PROVIDER 6u
#define LYRA_INTERFACE_MODES 8u
#define LYRA_INTERFACE_CONFIG_SCHEMA 9u
#define LYRA_INTERFACE_VERSION 3u
#define LYRA_PLAYBACK_ACTIVE 1u
#define LYRA_PLAYBACK_PAUSED 2u
#define LYRA_PLAYBACK_CLOCK_READY 4u
#define LYRA_PLAYBACK_SEEKING 8u
#define LYRA_PLAYBACK_FEATURES_READY 16u

typedef struct LyraPlaybackSnapshotV3 {
    uint32_t size, flags;
    int64_t track_id, position_us, duration_us, sampled_ns, output_frame;
    uint64_t generation, revision, feature_epoch, seek_serial, landed_seek;
    int32_t sample_rate;
    float energy, bass, mid, high, accent;
} LyraPlaybackSnapshotV3;
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
typedef struct LyraPlaybackCommandV3 {
    uint32_t size, kind, flags, seek_origin;
    int64_t track_id, position_us, duration_us;
    uint64_t generation;
    int32_t fd, index, capacity_ms, bursts;
    float bass_hz, wide_hz, hop_ms, accent_gain;
} LyraPlaybackCommandV3;
// event 0 EOF, 1 failure, 2 actual audio progress, 3 seek failure.
// The optional snapshot is copied during the call and is never retained.
typedef void (*LyraPlaybackEventFn)(void *, int32_t, uint64_t, const LyraPlaybackSnapshotV3 *);
// snapshot writes a value; no returned pointer may refer to a provider's worker data.
// OPEN transfers fd ownership. STOP is followed by quiesce before replacement.
// quiesce returns 1 only after every provider callback/worker has exited.
typedef struct LyraPlaybackProviderV3 {
    uint32_t size, version;
    void *user;
    int (*snapshot)(void *, int64_t, int32_t, LyraPlaybackSnapshotV3 *);
    int (*command)(void *, const LyraPlaybackCommandV3 *);
    int (*quiesce)(void *);
    void (*listener)(void *, LyraPlaybackEventFn, void *);
    int (*lyrics)(void *, uint64_t, int64_t, char *, int);
} LyraPlaybackProviderV3;
typedef struct LyraPlaybackInterfaceV3 {
    uint32_t size, version;
    void *user;
    int (*snapshot)(void *, LyraPlaybackSnapshotV3 *);
    int (*command)(void *, const LyraPlaybackCommandV3 *);
    int (*queue)(void *, int32_t *, int32_t, int32_t *);
} LyraPlaybackInterfaceV3;
typedef struct LyraLyricLineV3 {
    uint32_t size, flags;
    int32_t index, word_count, backing_count;
    int64_t start_us, end_us;
} LyraLyricLineV3;
typedef struct LyraLyricWordV3 {
    uint32_t size;
    int32_t index, units;
    int64_t start_us, end_us;
} LyraLyricWordV3;
typedef struct LyraLyricsInterfaceV3 {
    uint32_t size, version;
    void *user;
    uint64_t (*revision)(void *);
    int64_t (*track_id)(void *);
    int (*count)(void *);
    int (*line)(void *, int32_t, LyraLyricLineV3 *);
    // kind: 0 main, 1 translation, 2 phonetic, 3 backing, 4 word text,
    // 5 word phonetic; part selects backing/word. Return required bytes;
    // a too-small buffer is left untouched. All text is UTF-8.
    int (*text)(void *, int32_t, int32_t, int32_t, char *, int32_t);
    int (*word)(void *, int32_t, int32_t, LyraLyricWordV3 *);
} LyraLyricsInterfaceV3;
struct LyraHostV3;
struct LyraInput;
typedef struct LyraStyleDescriptionV3 {
    uint32_t size;
    char id[64], label[64];
    int32_t background_variant;
    void *user;
    int (*paint)(void *, const struct LyraHostV3 *, int64_t, float, float, float, float);
    int (*input)(void *, const struct LyraHostV3 *, const struct LyraInput *);
} LyraStyleDescriptionV3;
typedef struct LyraStylesInterfaceV3 {
    uint32_t size, version;
    void *user;
    int (*count)(void *);
    int (*describe)(void *, int32_t, LyraStyleDescriptionV3 *);
} LyraStylesInterfaceV3;
typedef struct LyraConfigDescriptionV3 {
    uint32_t size, kind;
    char key[128], value[256], default_value[256];
} LyraConfigDescriptionV3;
typedef struct LyraConfigInterfaceV3 {
    uint32_t size, version;
    void *user;
    int (*count)(void *);
    int (*describe)(void *, int32_t, LyraConfigDescriptionV3 *);
    // The value is a TOML scalar. Validation and activation happen at a frame boundary.
    int (*write)(void *, const char *, const char *);
} LyraConfigInterfaceV3;
typedef struct LyraPlaybackRuleRequestV3 {
    uint32_t size;
    int32_t mode, count, current, delta, automatic;
    int64_t entropy;
} LyraPlaybackRuleRequestV3;
typedef struct LyraPlaybackRuleResultV3 {
    uint32_t size;
    int32_t row, remember;
} LyraPlaybackRuleResultV3;
struct LyraModesInterfaceV3;
typedef struct LyraPlaybackRulesV3 {
    uint32_t size, version;
    void *user;
    int (*next)(void *, const LyraPlaybackRuleRequestV3 *, LyraPlaybackRuleResultV3 *);
    int (*order)(void *, const LyraPlaybackRuleRequestV3 *, int32_t *, int32_t, int32_t *);
    const struct LyraModesInterfaceV3 *modes; // Optional custom mode catalog; IDs >= 4.
} LyraPlaybackRulesV3;

typedef struct LyraModeDescriptionV3 {
    uint32_t size;
    int32_t id;
    char label[128];
} LyraModeDescriptionV3;
typedef struct LyraModesInterfaceV3 {
    uint32_t size, version;
    void *user;
    int (*count)(void *);
    int (*describe)(void *, int32_t, LyraModeDescriptionV3 *);
} LyraModesInterfaceV3;

#define LYRA_CONFIG_MINIMUM 1u
#define LYRA_CONFIG_MAXIMUM 2u
#define LYRA_CONFIG_INTEGER 4u
// Strings returned by declarations are borrowed only for the describe call.
typedef struct LyraParameterV3 {
    uint32_t size, kind, flags, schema_version;
    const char *key, *name, *description, *unit, *group, *default_value;
    double minimum, maximum;
} LyraParameterV3;
typedef struct LyraConfigSchemaV3 {
    uint32_t size, version;
    void *user;
    int (*count)(void *);
    int (*describe)(void *, int32_t, LyraParameterV3 *);
    // field 0 value, 1 default, 2 name, 3 description, 4 unit, 5 group.
    // Host interface only: required UTF-8 bytes including NUL, no partial writes.
    int (*text)(void *, const char *, int32_t, char *, int32_t);
} LyraConfigSchemaV3;

#define LYRA_INTERFACE_PLUGIN_CATALOG 10u
// state: 0 available, 1 installed/current, 2 downloading, 3 installing, 4
// update available.
typedef struct LyraPluginEntryV3 {
    uint32_t size, abi, state;
    uint64_t bytes;
    char id[64], title[128], version[32], description[256], capability[24];
} LyraPluginEntryV3;
// Entry indices belong to the current revision. install transfers the validated
// archive to the same package installer; source implementations own networking.
typedef struct LyraPluginCatalogV3 {
    uint32_t size, version;
    void *user;
    uint64_t (*revision)(void *);
    int (*count)(void *);
    int (*describe)(void *, int32_t, LyraPluginEntryV3 *);
    int (*search)(void *, const char *, int32_t *, int32_t);
    int (*install)(void *, int32_t);
    void (*refresh)(void *);
    int (*score)(void *,const char*,int32_t);
} LyraPluginCatalogV3;

// Plugin metadata and package actions.
#define LYRA_INTERFACE_PLUGIN_DETAILS 11u
// UTF-8 text fields; text() returns required bytes including NUL, never
// truncates. action: 0 install/update, 1 uninstall. installed identifies local
// metadata.
typedef struct LyraPluginDetailsV3 {
  uint32_t size, version;
  void *user;
  int (*text)(void *, const char *id, const char *field, int installed, char *,
              int);
  int (*action)(void *, const char *id, int action);
  int64_t (*icon_key)(void *, const char *id, int installed);
} LyraPluginDetailsV3;

// Classification rules.
#define LYRA_INTERFACE_CLASSIFICATION_RULES 12u
typedef struct LyraClassificationRulesV3 {
  uint32_t size, version;
  void *user;
  uint64_t (*revision)(void *);
  // UTF-8 NUL-terminated, required bytes; insufficient buffers remain
  // untouched.
  int (*genres)(void *, const char *, char *, int); // newline-separated labels
  int (*language)(void *, const char *primary_lyrics, const char *title,
                  int has_lyrics, char *, int);
  int (*language_label)(void *, const char *, char *, int);
} LyraClassificationRulesV3;

#include "plugin/media_contracts.h"
#include "plugin/event_contracts.h"

#include "service_contracts.h"

#include "presentation_contracts.h"

#define LYRA_INTERFACE_PLUGIN_INVENTORY 18u
struct LyraServiceView;
typedef struct LyraPluginInventoryV3 {
    uint32_t size, version;
    void *user;
    int (*list)(void *, struct LyraServiceView *, int capacity, int external);
} LyraPluginInventoryV3;
