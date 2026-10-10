#pragma once
#include "playback/audio_output.h"
#include <cstdint>
namespace lyra::audio {
struct Stream;
using Result = int32_t;
enum State : int32_t {
    Uninitialized = 0,
    Unknown,
    Open,
    Starting,
    Started,
    Pausing,
    Paused,
    Flushing,
    Flushed,
    Stopping,
    Stopped,
    Closing,
    Closed,
    Disconnected
};
constexpr Result Ok = 0, ErrorDisconnected = -899, ErrorInternal = -896, ErrorInvalidState = -895, ErrorTimeout = -885;
using ErrorCallback = void (*)(void *, Result);
Stream *open(int preferred_rate, const AudioOutputSettings &, ErrorCallback, void *);
Result close(Stream *);
int sample_rate(Stream *);
int xruns(Stream *);
int capacity(Stream *);
int size(Stream *);
int burst(Stream *);
int set_size(Stream *, int);
int64_t written(Stream *);
State state(Stream *);
Result wait_state(Stream *, State, State *, int64_t timeout_ns);
Result start(Stream *);
Result pause(Stream *);
Result flush(Stream *);
Result stop(Stream *);
Result write(Stream *, const void *, int frames, int64_t timeout_ns);
Result timestamp(Stream *, int64_t *presented_frame, int64_t *monotonic_ns);
} // namespace lyra::audio
