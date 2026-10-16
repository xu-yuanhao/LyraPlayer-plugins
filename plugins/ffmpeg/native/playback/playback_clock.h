#pragma once
#include "playback/audio_features.h"
#include "playback/clock_math.h"
#include "playback/pcm_timeline.h"
#include <mutex>
#include <utility>

// Public media time is always microseconds. Output frames stay in the hardware
// domain; their mapping is replaced atomically at a track/seek boundary.
struct PlaybackSnapshot {
    int64_t track_id = -1, position_us = 0, duration_us = 0, sampled_ns = 0, output_frame = 0;
    uint64_t generation = 0, revision = 0, feature_epoch = 0, seek_serial = 0, landed_seek = 0;
    int sample_rate = 0;
    bool active = false, paused = false, clock_ready = false, seeking = false, features_ready = false;
    AudioFeatures features;
    bool playing() const { return active && !paused; }
};

struct PlaybackClockState {
    int64_t track_id = -1, duration_us = 0, origin_us = 0, origin_frame = 0;
    int64_t presented = 0, stamp_ns = 0, submitted = 0, restart_frame = 0, restart_ns = 0, frozen_frame = 0, frozen_us = 0;
    uint64_t generation = 0, revision = 0, feature_epoch = 0, seek_serial = 0, landed_seek = 0;
    int rate = 0;
    bool active = false, paused = false, ready = false, timestamp_valid = false, seeking = false;
    PcmTimeline pcm;
    mutable int64_t sampled_frame = 0;

    PlaybackSnapshot sample(int64_t now, bool commit = true) const {
        PlaybackSnapshot s;
        s.track_id = track_id; s.duration_us = duration_us; s.sampled_ns = now;
        s.generation = generation; s.revision = revision; s.feature_epoch = feature_epoch;
        s.seek_serial = seek_serial; s.landed_seek = landed_seek;
        s.sample_rate = rate; s.active = active; s.paused = paused; s.clock_ready = timestamp_valid; s.seeking = seeking;
        bool pending_seek=seeking && landed_seek!=seek_serial;
        s.output_frame = paused || !active || pending_seek ? frozen_frame : (ready ? (timestamp_valid ? audio_presentation_frame(
            rate, presented, stamp_ns, submitted, restart_frame, restart_ns, now, true) : restart_frame) : origin_frame);
        if (active && !paused && !pending_seek && ready) {
            // Hardware timestamps can correct interpolation slightly backwards.
            // Hold the last audio frame until the hardware catches up; do not
            // create a second, independently advancing media timer.
            s.output_frame = std::min(std::max(s.output_frame, sampled_frame), std::max(restart_frame, submitted));
            if (commit) sampled_frame = s.output_frame;
        }
        s.position_us = origin_us + audio_frames_us(std::max<int64_t>(0, s.output_frame - origin_frame), rate);
        s.position_us = pcm.position(s.output_frame, rate, s.position_us);
        if (pending_seek) s.position_us = frozen_us;
        else if (paused || !active) s.position_us = ready ? frozen_us : origin_us;
        if (duration_us > 0) s.position_us = std::min(s.position_us, duration_us);
        return s;
    }
    PlaybackSnapshot for_presentation(int64_t observed_ns, int64_t present_ns) const {
        auto observed = sample(observed_ns);
        // Only observations at real wall time advance the monotonic floor.
        // A picture is displayed later, but pause/seek/bookmarks must never
        // inherit that prediction as though its audio had already played.
        return present_ns > observed_ns ? sample(present_ns, false) : observed;
    }
    void freeze(int64_t now) {
        auto s = sample(now);
        frozen_frame = s.output_frame; frozen_us = s.position_us;
    }
    void restart(int64_t frame, int64_t now) {
        restart_frame = presented = frozen_frame = frame;
        restart_ns = stamp_ns = now;
        submitted = std::max(frame, submitted);
        ready = true;
        timestamp_valid = false;
        sampled_frame = frame;
    }
    void publish(int64_t frame, int64_t stamp, int64_t queued, bool valid) {
        submitted = queued;
        if (!ready || !valid || frame < restart_frame || stamp < restart_ns) return;
        if (timestamp_valid && (frame < presented || stamp < stamp_ns)) return;
        presented = frame; stamp_ns = stamp; timestamp_valid = true;
    }
};

// No lock is held during decoding, stream writes, platform calls, or disk I/O.
// The blocking audio writer is the producer; there is no realtime callback here.
class PlaybackClock {
  public:
    template<class F> auto access(F fn) const -> decltype(fn(std::declval<PlaybackClockState&>())) {
        std::lock_guard<std::mutex> lock(mutex_);
        return fn(state_);
    }
    PlaybackSnapshot sample(int64_t now) const { return access([&](const PlaybackClockState& s) { return s.sample(now); }); }
  private:
    mutable std::mutex mutex_;
    mutable PlaybackClockState state_;
};
