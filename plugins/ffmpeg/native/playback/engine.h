#pragma once

#include "playback/progress_memory.h"
#include "playback/audio_features.h"
#include "playback/playback_clock.h"

#include "platform/audio_output.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <semaphore.h>
#include <string>

#include "playback/audio_output.h"

class Engine {
public:
    static constexpr int kEnded = 0;
    static constexpr int kFailed = 1;
    enum SeekOrigin { kTransportSeek, kProgressSeek, kLyricSeek, kOpenSeek, kRestoreSeek, kRecoverySeek };

    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void set_memory(ProgressMemory* memory){memory_=memory?memory:&private_memory_;}
    void set_listener(void (*fn)(void* user, int event, uint64_t generation), void* user);
    void start(int fd, int64_t track, int64_t duration, const AudioFeatureSettings& settings, uint64_t generation,
               const AudioOutputSettings& output = {});
    PlaybackSnapshot snapshot(int64_t time_ns = 0, bool with_features = true) const;
    PlaybackBookmark configure_memory(const std::string& path, int interval_ms);
    void remember_selection(int64_t track, int64_t position, int64_t duration,uint64_t generation=0);
    void checkpoint_progress();
    AudioFeatures audio_features(int64_t time_ns, bool* ready = nullptr) const;
    void pause();
    bool resume();
    bool paused() const;
    void seek(int64_t ms, SeekOrigin origin = kTransportSeek);
    void prime_start(int64_t ms, bool paused);
    void clear_prime();
    bool take_seek_failed();
    void stop();
    void reap();
    bool timer_active() const;
    double position_exact() const;
    int64_t position_ms() const;
    void log_underruns();
    bool take_lyrics(std::string* out, uint64_t generation, int64_t track);

private:
    enum Exit { kNone = 0, kUser = 1 };

    static void* tramp(void* self);
    static void error_cb(void* user, lyra::audio::Result error);

    void decode_main();
    void on_error(lyra::audio::Result error);
    bool open_output(int preferred_rate);
    void release_writer(bool keep);
    bool discard_pending(lyra::audio::Stream* stream);
    void close_stream();
    void feed(const int16_t* src, int count, int64_t frame_us);
    void wait_for_drain();
    void maybe_start();
    void publish_lyrics(std::string text);

    int64_t raw_position_ms() const;
    void reset_features(int rate);
    void refresh_clock(lyra::audio::Stream* stream);
    ProgressMemory private_memory_;
    ProgressMemory* memory_=&private_memory_;
    AudioFeatureHistory features_;
    AudioFeatureSettings feature_settings_;
    AudioOutputSettings output_settings_;
    AudioOutputSettings stream_settings_;
    int64_t pcm_origin_us_ = -1, pcm_frames_ = 0;
    sem_t sem_{};
    pthread_t thread_{};
    std::atomic<bool> thread_live_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> fault_{false};
    std::atomic<bool> recover_output_{false};
    std::atomic<bool> finished_{true};
    std::atomic<bool> started_{false};
    std::atomic<int> exit_{kNone};
    std::atomic<int> rate_{0};
    PlaybackClock clock_;
    std::recursive_mutex stream_mu_;
    std::atomic<int64_t> seek_ms_{-1};
    SeekOrigin seek_origin_ = kTransportSeek; // guarded by clock_
    std::atomic<int64_t> start_at_{-1};
    std::atomic<bool> start_paused_{false};
    std::atomic<bool> seek_failed_{false};
    int64_t drop_until_us_ = -1;
    int drop_blind_ = 0;
    std::atomic<int> underruns_{0};
    std::atomic<int> fade_left_{0};
    std::atomic<int> fade_total_{0};
    std::atomic<lyra::audio::Stream*> stream_{nullptr};
    std::atomic<bool> keep_stream_{false};
    int fd_ = -1;
    std::atomic<int64_t> decode_mark_{0};
    bool underrun_logged_ = false;
    int last_xruns_ = 0;
    void (*listener_)(void*, int, uint64_t) = nullptr;
    void* listener_user_ = nullptr;
    std::mutex lyrics_mu_;
    std::string lyrics_text_;
    std::atomic<bool> lyrics_pending_{false};
};

std::string read_embedded_lyrics(int fd);
