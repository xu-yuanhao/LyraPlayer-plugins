#pragma once
#include "playback/playback_clock.h"
#include "playback/progress_memory.h"
#include "plugin/contracts.h"
#include <string>
// Native-only ownership extension for the built-in engine. Plugin providers use
// the C protocol; their lifetime is retained by the registry until quiescence.
struct OwnedPlaybackBackend {
    LyraPlaybackProviderV2 api{};
    void (*destroy)(void *) = nullptr;
    void (*attach_memory)(void *, ProgressMemory *) = nullptr;
    bool (*seek_failed)(void *) = nullptr;
    void (*underruns)(void *) = nullptr;
};
OwnedPlaybackBackend make_engine_backend();
inline LyraPlaybackSnapshotV2 playback_export(const PlaybackSnapshot &s) {
    LyraPlaybackSnapshotV2 out{};
    out.size = sizeof(out);
    out.flags = (s.active ? LYRA_PLAYBACK_ACTIVE : 0) | (s.paused ? LYRA_PLAYBACK_PAUSED : 0) |
                (s.clock_ready ? LYRA_PLAYBACK_CLOCK_READY : 0) | (s.seeking ? LYRA_PLAYBACK_SEEKING : 0) |
                (s.features_ready ? LYRA_PLAYBACK_FEATURES_READY : 0);
    out.track_id = s.track_id;
    out.position_us = s.position_us;
    out.duration_us = s.duration_us;
    out.sampled_ns = s.sampled_ns;
    out.output_frame = s.output_frame;
    out.generation = s.generation;
    out.revision = s.revision;
    out.feature_epoch = s.feature_epoch;
    out.seek_serial = s.seek_serial;
    out.landed_seek = s.landed_seek;
    out.sample_rate = s.sample_rate;
    out.energy = s.features.energy;
    out.bass = s.features.bass;
    out.mid = s.features.mid;
    out.high = s.features.high;
    out.accent = s.features.accent;
    return out;
}
inline PlaybackSnapshot playback_import(const LyraPlaybackSnapshotV2 &s) {
    PlaybackSnapshot out;
    out.track_id = s.track_id;
    out.position_us = s.position_us;
    out.duration_us = s.duration_us;
    out.sampled_ns = s.sampled_ns;
    out.output_frame = s.output_frame;
    out.generation = s.generation;
    out.revision = s.revision;
    out.feature_epoch = s.feature_epoch;
    out.seek_serial = s.seek_serial;
    out.landed_seek = s.landed_seek;
    out.sample_rate = s.sample_rate;
    out.active = s.flags & LYRA_PLAYBACK_ACTIVE;
    out.paused = s.flags & LYRA_PLAYBACK_PAUSED;
    out.clock_ready = s.flags & LYRA_PLAYBACK_CLOCK_READY;
    out.seeking = s.flags & LYRA_PLAYBACK_SEEKING;
    out.features_ready = s.flags & LYRA_PLAYBACK_FEATURES_READY;
    out.features = {s.energy, s.bass, s.mid, s.high, s.accent};
    return out;
}
