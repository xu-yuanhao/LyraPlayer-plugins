#pragma once
#include <algorithm>
#include <cstdint>
#include <chrono>

inline int64_t audio_ms_us(int64_t milliseconds) {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::milliseconds(milliseconds)).count();
}

inline int64_t audio_frames_us(int64_t frames, int rate) {
    if (rate < 1 || frames < 1) return 0;
    return (frames / rate) * 1000000LL + (frames % rate) * 1000000LL / rate;
}

// A hardware stamp can be cached across pause/flush, or update only once per burst.
// Extrapolate from the current playback segment, bounded by submitted PCM.
inline int64_t audio_presentation_frame(int rate, int64_t frame, int64_t stamp_ns, int64_t written,
                                        int64_t restart_frame, int64_t restart_ns, int64_t now_ns, bool playing) {
    if (stamp_ns < restart_ns || frame < restart_frame) {
        frame = restart_frame;
        stamp_ns = restart_ns;
    }
    if (rate > 0 && stamp_ns > 0 && playing) {
        double extra = (double)(now_ns - stamp_ns) * rate / 1e9;
        extra = std::clamp(extra, -(double)frame, (double)std::max(written, frame) - frame);
        frame += (int64_t)extra;
    }
    return std::clamp(frame, restart_frame, std::max(restart_frame, written));
}
inline int64_t audio_media_frame(int rate, int64_t origin_ms, int64_t origin_frame, int64_t frame) {
    if (rate < 1 || frame < 0)
        return -1;
    return origin_ms * rate / 1000 + std::max<int64_t>(0, frame - origin_frame);
}
