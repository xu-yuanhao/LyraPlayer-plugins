#pragma once
#include "playback/clock_math.h"
#include <array>
#include <cstdlib>
#include <cmath>

// Map the PCM actually submitted to the output to its decoded media time. A
// device underrun/flush can leave holes in output counters; container PTS can
// also jump. Neither hole is time spent playing the song. Contiguous writes
// coalesce, so hours of ordinary playback still occupy just one segment.
class PcmTimeline {
  public:
    void reset() { count_ = next_ = 0; }
    void append(int64_t first, int frames, int64_t media_us, int rate, double speed=1) {
        if (frames < 1 || rate < 1 || !std::isfinite(speed) || speed<=0) return;
        if (count_ > 0) {
            auto& last = segments_[(next_ + capacity - 1) % capacity];
            int64_t expected = last.media_us + int64_t(double(first-last.first)*1000000/rate*last.speed);
            if (speed==last.speed && first == last.end && std::llabs(expected - media_us) <= audio_frames_us(2, rate)) {
                last.end += frames;
                return;
            }
        }
        segments_[next_] = {first, first + frames, media_us,speed};
        next_ = (next_ + 1) % capacity;
        count_ = std::min(count_ + 1, capacity);
    }
    int64_t position(int64_t frame, int rate, int64_t fallback_us) const {
        for (int i = 0; i < count_; ++i) {
            const auto& s = segments_[(next_ + capacity - 1 - i) % capacity];
            if (frame >= s.first)
                return s.media_us + int64_t(double(std::min(frame,s.end)-s.first)*1000000/rate*s.speed);
        }
        return fallback_us;
    }
  private:
    struct Segment { int64_t first = 0, end = 0, media_us = 0; double speed=1; };
    static constexpr int capacity = 256;
    std::array<Segment, capacity> segments_{};
    int count_ = 0, next_ = 0;
};
