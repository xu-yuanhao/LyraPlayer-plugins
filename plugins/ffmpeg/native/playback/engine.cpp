#include "playback/engine.h"
#include "playback/clock_math.h"
#include <algorithm>

#include "platform/log.h"

#include <cstdarg>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
}

namespace {

std::atomic<int> g_log_ready{0};
std::mutex g_ffmpeg_open;

bool key_has(const char* key, const char* needle) {
    if (key == nullptr || needle == nullptr) return false;
    for (const char* p = key; *p != '\0'; ++p) {
        const char* a = p;
        const char* b = needle;
        while (*a != '\0' && *b != '\0') {
            char c = *a;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != *b) break;
            ++a;
            ++b;
        }
        if (*b == '\0') return true;
    }
    return false;
}

bool lyric_metadata_key(const char* key) {
    return key_has(key, "lyric") || key_has(key, "uslt") || key_has(key, "sylt");
}

void consider_lyric_dict(AVDictionary* dict, std::string* best, bool* best_synced, const char** best_key) {
    if (dict == nullptr) return;
    const AVDictionaryEntry* entry = nullptr;
    while ((entry = av_dict_get(dict, "", entry, AV_DICT_IGNORE_SUFFIX)) != nullptr) {
        if (!lyric_metadata_key(entry->key) || entry->value == nullptr || entry->value[0] == '\0') continue;
        size_t bytes = std::strlen(entry->value);
        if (bytes > 4u * 1024u * 1024u) continue;
        bool synced = std::strchr(entry->value, '[') != nullptr || std::strstr(entry->value, "<tt") != nullptr;
        bool better = best->empty() || (synced && !*best_synced) || (synced == *best_synced && bytes > best->size());
        if (!better) continue;
        best->assign(entry->value, bytes);
        *best_synced = synced;
        *best_key = entry->key;
    }
}

std::string lyrics_from_context(AVFormatContext* fmt) {
    std::string best;
    bool synced = false;
    const char* key = "";
    if (fmt == nullptr) return best;
    consider_lyric_dict(fmt->metadata, &best, &synced, &key);
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i] != nullptr) consider_lyric_dict(fmt->streams[i]->metadata, &best, &synced, &key);
    }
    if (best.empty()) {
        lyra_log(LyraLogInfo, "Lyra", "lyrics metadata none");
    } else {
        lyra_log(LyraLogInfo, "Lyra", "lyrics metadata key=%s bytes=%zu synced=%d", key, best.size(),
                            synced ? 1 : 0);
    }
    return best;
}

void ffmpeg_log(void*, int level, const char* fmt, va_list ap) {
    if (level > AV_LOG_ERROR) return;
    char line[256];
    std::vsnprintf(line, sizeof(line), fmt, ap);
    lyra_log(LyraLogInfo, "Lyra", "ffmpeg %s", line);
}

void arm_log() {
    if (g_log_ready.exchange(1, std::memory_order_relaxed) != 0) return;
    av_log_set_level(AV_LOG_ERROR);
    av_log_set_callback(ffmpeg_log);
}

int64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
int64_t now_ms() { return now_ns() / 1000000; }

bool settle_stream(lyra::audio::Stream* stream) {
    for (int i = 0; i < 4; ++i) {
        auto state = lyra::audio::state(stream);
        if (state != lyra::audio::Starting && state != lyra::audio::Pausing &&
            state != lyra::audio::Flushing && state != lyra::audio::Stopping)
            return state != lyra::audio::Disconnected && state != lyra::audio::Closed;
        lyra::audio::State next = state;
        lyra::audio::wait_state(stream, state, &next, 50000000LL);
    }
    return false;
}

void log_fail(const char* what, int err) {
    char buf[64];
    buf[0] = 0;
    if (err < 0) av_strerror(err, buf, sizeof(buf));
    lyra_log(LyraLogInfo, "Lyra", "play fail %s %s", what, buf);
}

int64_t frame_media_us(AVFrame* frame, AVStream* stream) {
    if (frame == nullptr || stream == nullptr) return -1;
    int64_t pts = frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) pts = frame->pts;
    if (pts == AV_NOPTS_VALUE) return -1;
    if (stream->start_time != AV_NOPTS_VALUE) pts -= stream->start_time;
    return std::max<int64_t>(0, av_rescale_q(pts, stream->time_base, AVRational{1, 1000000}));
}

struct DecodeSession {
    int fd = -1;
    AVIOContext* avio = nullptr;
    AVFormatContext* fmt = nullptr;
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    SwrContext* swr = nullptr;
    int swr_rate = -1;
    int swr_fmt = -1;
    int swr_ch = -1;
    std::vector<int16_t> pcm;

    ~DecodeSession() {
        if (ctx != nullptr) avcodec_free_context(&ctx);
        if (frame != nullptr) av_frame_free(&frame);
        if (pkt != nullptr) av_packet_free(&pkt);
        if (fmt != nullptr) avformat_close_input(&fmt);
        if (avio != nullptr) {
            av_freep(&avio->buffer);
            avio_context_free(&avio);
        }
        if (swr != nullptr) swr_free(&swr);
        if (fd >= 0) ::close(fd);
    }
};

int io_read(void* opaque, uint8_t* buf, int size) {
    auto* session = static_cast<DecodeSession*>(opaque);
    if (size <= 0) return AVERROR(EINVAL);
    for (;;) {
        ssize_t n = ::read(session->fd, buf, (size_t)size);
        if (n == 0) return AVERROR_EOF;
        if (n > 0) return (int)n;
        if (errno == EINTR) continue;
        return AVERROR(errno);
    }
}

int64_t io_seek(void* opaque, int64_t offset, int whence) {
    auto* session = static_cast<DecodeSession*>(opaque);
    if ((whence & AVSEEK_SIZE) != 0) {
        struct stat st {};
        if (fstat(session->fd, &st) != 0) return AVERROR(errno);
        return st.st_size;
    }
    int base = whence & ~AVSEEK_FORCE;
    if (base != SEEK_SET && base != SEEK_CUR && base != SEEK_END) return AVERROR(EINVAL);
    off_t pos = ::lseek(session->fd, (off_t)offset, base);
    if (pos == (off_t)-1) return AVERROR(errno);
    return pos;
}

}  // namespace

std::string read_embedded_lyrics(int fd) {
    std::string best;
    if (fd < 0) return best;
    arm_log();
    std::lock_guard<std::mutex> open_lock(g_ffmpeg_open);
    DecodeSession session;
    session.fd = fd;
    uint8_t* buffer = static_cast<uint8_t*>(av_malloc(32768));
    if (buffer == nullptr) return best;
    session.avio = avio_alloc_context(buffer, 32768, 0, &session, io_read, nullptr, io_seek);
    if (session.avio == nullptr) {
        av_free(buffer);
        return best;
    }
    session.fmt = avformat_alloc_context();
    if (session.fmt == nullptr) return best;
    session.fmt->pb = session.avio;
    session.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    session.fmt->probesize = 96 * 1024;
    session.fmt->format_probesize = 96 * 1024;
    if (avformat_open_input(&session.fmt, nullptr, nullptr, nullptr) < 0) {
        session.fmt = nullptr;
        return best;
    }
    bool synced = false;
    const char* key = "";
    consider_lyric_dict(session.fmt->metadata, &best, &synced, &key);
    for (unsigned i = 0; session.fmt->streams != nullptr && i < session.fmt->nb_streams; ++i) {
        if (session.fmt->streams[i] != nullptr) consider_lyric_dict(session.fmt->streams[i]->metadata, &best, &synced, &key);
    }
    return best;
}

Engine::Engine() { sem_init(&sem_, 0, 0); }

Engine::~Engine() {
    stop();
    sem_destroy(&sem_);
}

void Engine::set_listener(void (*fn)(void* user, int event, uint64_t generation), void* user) {
    listener_ = fn;
    listener_user_ = user;
}

void* Engine::tramp(void* self) {
    static_cast<Engine*>(self)->decode_main();
    return nullptr;
}

void Engine::error_cb(void* user, lyra::audio::Result error) {
    static_cast<Engine*>(user)->on_error(error);
}

void Engine::start(int fd, int64_t track, int64_t duration, const AudioFeatureSettings& settings, uint64_t generation,
                   const AudioOutputSettings& output) {
    release_writer(true);
    {
        std::lock_guard<std::mutex> lock(lyrics_mu_);
        lyrics_text_.clear();
        lyrics_pending_.store(false, std::memory_order_release);
    }
    keep_stream_.store(false, std::memory_order_release);
    while (sem_trywait(&sem_) == 0) {
    }
    feature_settings_ = settings;
    output_settings_ = output;
    pcm_origin_us_ = -1; pcm_frames_ = 0;
    stop_.store(false, std::memory_order_relaxed);
    paused_.store(false, std::memory_order_relaxed);
    fault_.store(false, std::memory_order_relaxed);
    recover_output_.store(false, std::memory_order_relaxed);
    finished_.store(false, std::memory_order_relaxed);
    started_.store(false, std::memory_order_relaxed);
    exit_.store(kNone, std::memory_order_relaxed);
    seek_ms_.store(-1, std::memory_order_relaxed);
    bool primed_paused = start_paused_.exchange(false, std::memory_order_acq_rel);
    paused_.store(primed_paused, std::memory_order_release);
    int64_t primed = start_at_.exchange(-1, std::memory_order_acq_rel);
    if (primed >= 0) seek_ms_.store(primed, std::memory_order_release);
    clock_.access([&](PlaybackClockState& c) {
        uint64_t revision = c.revision + 1;
        c = {};
        c.track_id = track; c.generation = generation; c.revision = revision;
        c.duration_us = std::max<int64_t>(0, duration) * 1000;
        c.origin_us = std::max<int64_t>(0, primed) * 1000;
        c.active = fd >= 0; c.paused = primed_paused; c.seeking = primed >= 0;
        c.seek_serial = primed >= 0 ? 1 : 0;
        seek_origin_ = kRestoreSeek;
    });
    if (fd < 0) {
        close_stream();
        finished_.store(true, std::memory_order_release);
        if (listener_) listener_(listener_user_, kFailed, generation);
        return;
    }
    memory_->select(track, std::max<int64_t>(0, primed), duration);
    drop_until_us_ = -1;
    drop_blind_ = 0;
    rate_.store(0, std::memory_order_relaxed);
    underruns_.store(0, std::memory_order_relaxed);
    fade_left_.store(0, std::memory_order_relaxed);
    fade_total_.store(0, std::memory_order_relaxed);
    underrun_logged_ = false;
    fd_ = fd;
    if (pthread_create(&thread_, nullptr, &Engine::tramp, this) != 0) {
        ::close(fd);
        fd_ = -1;
        finished_.store(true, std::memory_order_release);
        clock_.access([](PlaybackClockState& c) { c.active = false; });
        close_stream();
        if (listener_) listener_(listener_user_, kFailed, generation);
        lyra_log(LyraLogInfo, "Lyra", "play fail thread");
        return;
    }
    thread_live_.store(true, std::memory_order_release);
}

PlaybackBookmark Engine::configure_memory(const std::string& path, int interval_ms) { return memory_->configure(path, interval_ms); }
void Engine::remember_selection(int64_t track, int64_t position, int64_t duration,uint64_t generation) {
    memory_->select(track, position, duration);
    clock_.access([&](PlaybackClockState& c) {
        if (c.active) return;
        if(generation)c.generation=generation;
        c.track_id = track; c.duration_us = std::max<int64_t>(0, duration) * 1000;
        c.origin_us = c.frozen_us = std::max<int64_t>(0, position) * 1000;
        c.origin_frame = c.frozen_frame = c.sampled_frame = 0;
        c.ready = c.timestamp_valid = false; c.pcm.reset();
    });
}
PlaybackSnapshot Engine::snapshot(int64_t time_ns, bool with_features) const {
    const int64_t observed_ns = now_ns();
    if (time_ns <= 0) time_ns = observed_ns;
    return clock_.access([&](const PlaybackClockState& c) {
        PlaybackSnapshot s = c.for_presentation(observed_ns, time_ns);
        if (with_features && s.playing() && !s.seeking && s.clock_ready && s.output_frame < c.submitted &&
            c.feature_epoch == features_.epoch()) {
            // A slot can be published while reading; retry without re-reading the clock.
            for (int i = 0; i < 3 && !s.features_ready; ++i)
                s.features = features_.sample_output(s.output_frame, &s.features_ready);
        }
        return s;
    });
}
void Engine::reset_features(int rate) {
    clock_.access([&](PlaybackClockState& c) {
        features_.reset(rate, feature_settings_);
        c.rate = rate; c.feature_epoch = features_.epoch();
        rate_.store(rate, std::memory_order_release);
    });
}
int64_t Engine::raw_position_ms() const { return snapshot(0, false).position_us / 1000; }
void Engine::checkpoint_progress() {
    PlaybackSnapshot s = snapshot(0, false);
    if (s.active) { memory_->update(s.position_us / 1000); memory_->checkpoint(); }
}
AudioFeatures Engine::audio_features(int64_t time_ns, bool* ready) const {
    PlaybackSnapshot s = snapshot(time_ns);
    if (ready) *ready = s.features_ready;
    return s.features;
}

void Engine::stop() { release_writer(false); }

void Engine::release_writer(bool keep) {
    checkpoint_progress();
    int64_t mark = now_ms();
    bool live = thread_live_.load(std::memory_order_acquire) || stream_.load(std::memory_order_acquire) != nullptr;
    keep_stream_.store(keep, std::memory_order_release);
    exit_.store(kUser, std::memory_order_release);
    stop_.store(true, std::memory_order_release);
    lyra::audio::Stream* stream = nullptr;
    if (keep) {
        std::lock_guard<std::recursive_mutex> control(stream_mu_);
        stream = stream_.load(std::memory_order_acquire);
        if (stream != nullptr) {
            settle_stream(stream);
            lyra::audio::State state = lyra::audio::state(stream);
            if (state == lyra::audio::Started) lyra::audio::pause(stream);
        }
    }
    sem_post(&sem_);
    if (thread_live_.exchange(false, std::memory_order_acq_rel)) pthread_join(thread_, nullptr);
    if (keep) {
        stream = stream_.load(std::memory_order_acquire);
        if (stream != nullptr && !discard_pending(stream)) close_stream();
    } else {
        close_stream();
    }
    clock_.access([&](PlaybackClockState& c) {
        c.freeze(now_ns());
        c.active = false; c.seeking = false;
    });
    finished_.store(true, std::memory_order_release);
    if (live) lyra_log(LyraLogInfo, "Lyra", "play stop ms=%lld", (long long)(now_ms() - mark));
}

void Engine::reap() {
    if (!finished_.load(std::memory_order_acquire)) return;
    if (thread_live_.exchange(false, std::memory_order_acq_rel)) pthread_join(thread_, nullptr);
    close_stream();
}

void Engine::pause() {
    std::lock_guard<std::recursive_mutex> control(stream_mu_);
    lyra::audio::Stream* stream = stream_.load(std::memory_order_acquire);
    if (stream != nullptr && started_.load(std::memory_order_acquire) && !settle_stream(stream)) {
        on_error(lyra::audio::ErrorTimeout);
        return;
    }
    PlaybackSnapshot frozen = clock_.access([&](PlaybackClockState& c) {
        auto s = c.sample(now_ns());
        c.frozen_frame = s.output_frame; c.frozen_us = s.position_us; c.paused = true;
        paused_.store(true, std::memory_order_release);
        return s;
    });
    if (frozen.active) { memory_->update(frozen.position_us / 1000); memory_->checkpoint(); }
    if (stream != nullptr && started_.load(std::memory_order_acquire)) {
        auto rc = lyra::audio::pause(stream);
        if (rc != lyra::audio::Ok) on_error(rc);
    }
    if (frozen.active) lyra_log(LyraLogInfo, "Lyra", "play pause");
}

bool Engine::paused() const { return snapshot(0, false).paused; }

void Engine::seek(int64_t ms, SeekOrigin origin) {
    if (ms < 0) ms = 0;
    seek_failed_.store(false, std::memory_order_release);
    // The audible clock moves when the decoder lands, not when the request is posted.
    // Publishing the target here lets the lyric follower run ahead of the samples still in the speaker.
    PlaybackSnapshot before = clock_.access([&](PlaybackClockState& c) {
        auto s = c.sample(now_ns());
        c.seeking = true; ++c.seek_serial;
        seek_origin_ = origin;
        seek_ms_.store(ms, std::memory_order_release);
        return s;
    });
    lyra_log(LyraLogInfo, "Lyra", "play seek request origin=%d track=%lld gen=%llu from_us=%lld target_ms=%lld",
        int(origin), (long long)before.track_id, (unsigned long long)before.generation,
        (long long)before.position_us, (long long)ms);
    sem_post(&sem_);
}

void Engine::prime_start(int64_t ms, bool paused) {
    if (ms < 0) ms = 0;
    start_at_.store(ms, std::memory_order_release);
    start_paused_.store(paused, std::memory_order_release);
}

void Engine::clear_prime() {
    start_at_.store(-1, std::memory_order_release);
    start_paused_.store(false, std::memory_order_release);
}

bool Engine::take_seek_failed() { return seek_failed_.exchange(false, std::memory_order_acq_rel); }

bool Engine::resume() {
    std::lock_guard<std::recursive_mutex> control(stream_mu_);
    lyra::audio::Stream* stream = stream_.load(std::memory_order_acquire);
    if (stream != nullptr && started_.load(std::memory_order_acquire) && !settle_stream(stream)) {
        on_error(lyra::audio::ErrorTimeout);
        return false;
    }
    bool resumed = clock_.access([&](PlaybackClockState& c) {
        if (!c.paused || !c.active) return false;
        if (c.ready) c.restart(c.frozen_frame, now_ns());
        c.paused = false;
        paused_.store(false, std::memory_order_release);
        return true;
    });
    if (!resumed) return false;
    lyra_log(LyraLogInfo, "Lyra", "play resume");
    sem_post(&sem_);
    if (!started_.load(std::memory_order_acquire)) maybe_start();
    else {
        // EOF still has queued PCM: restarting its stream is essential to drain it.
        if (stream != nullptr) {
            auto rc = lyra::audio::start(stream);
            if (rc != lyra::audio::Ok) { on_error(rc); return false; }
        }
    }
    return true;
}

bool Engine::timer_active() const { return snapshot(0, false).active; }
double Engine::position_exact() const { return snapshot(0, false).position_us / 1000.; }
int64_t Engine::position_ms() const { return snapshot(0, false).position_us / 1000; }

void Engine::log_underruns() {
    int count = underruns_.exchange(0, std::memory_order_relaxed);
    if (count <= 0 || underrun_logged_) return;
    underrun_logged_ = true;
    lyra_log(LyraLogInfo, "Lyra", "play underrun %d", count);
}

void Engine::publish_lyrics(std::string text) {
    std::lock_guard<std::mutex> lock(lyrics_mu_);
    lyrics_text_ = std::move(text);
    lyrics_pending_.store(true, std::memory_order_release);
}

bool Engine::take_lyrics(std::string* out, uint64_t generation, int64_t track) {
    if (out == nullptr || !lyrics_pending_.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::mutex> lock(lyrics_mu_);
    auto s = snapshot(0, false);
    if (generation != s.generation || track != s.track_id) return false;
    if (!lyrics_pending_.exchange(false, std::memory_order_acq_rel)) return false;
    *out = std::move(lyrics_text_);
    lyrics_text_.clear();
    return true;
}

bool Engine::discard_pending(lyra::audio::Stream* stream) {
    auto wait = [&](lyra::audio::State target) {
        for (int i = 0; i < 4; ++i) {
            auto state = lyra::audio::state(stream);
            if (state == target) return true;
            if (state == lyra::audio::Disconnected || state == lyra::audio::Closed) return false;
            lyra::audio::State next = state;
            lyra::audio::wait_state(stream, state, &next, 50000000LL);
        }
        return lyra::audio::state(stream) == target;
    };
    auto state = lyra::audio::state(stream);
    if (state == lyra::audio::Open || state == lyra::audio::Flushed) return true;
    if (state == lyra::audio::Started || state == lyra::audio::Starting || state == lyra::audio::Pausing) {
        if (state != lyra::audio::Pausing && lyra::audio::pause(stream) != lyra::audio::Ok) return false;
        if (!wait(lyra::audio::Paused)) return false;
    }
    if (lyra::audio::flush(stream) != lyra::audio::Ok) return false;
    // A successful request is asynchronous; discarded PCM is gone only after FLUSHED.
    return wait(lyra::audio::Flushed);
}

void Engine::close_stream() {
    std::lock_guard<std::recursive_mutex> control(stream_mu_);
    lyra::audio::Stream* stream = stream_.exchange(nullptr, std::memory_order_acq_rel);
    if (stream == nullptr) return;
    if (exit_.load(std::memory_order_acquire) == kUser) discard_pending(stream);
    lyra::audio::stop(stream);
    lyra::audio::close(stream);
}

bool Engine::open_output(int preferred_rate) {
    std::lock_guard<std::recursive_mutex> control(stream_mu_);
    auto* existing=stream_.load(std::memory_order_acquire);
    if(existing&&preferred_rate>=8000&&lyra::audio::sample_rate(existing)==preferred_rate&&stream_settings_.capacity_ms==output_settings_.capacity_ms&&stream_settings_.bursts==output_settings_.bursts) {
        reset_features(preferred_rate);last_xruns_=lyra::audio::xruns(existing);lyra_log(LyraLogInfo,"Lyra","play stream reuse rate=%d",preferred_rate);return true;
    }
    if(existing)close_stream();
    auto* stream=lyra::audio::open(preferred_rate,output_settings_,&Engine::error_cb,this);
    if(!stream){log_fail("stream",0);return false;}
    stream_.store(stream,std::memory_order_release);stream_settings_=output_settings_;
    reset_features(lyra::audio::sample_rate(stream));last_xruns_=lyra::audio::xruns(stream);return true;
}

void Engine::feed(const int16_t* src, int count, int64_t frame_us) {
    if (count < 2 || src == nullptr) return;
    if (seek_ms_.load(std::memory_order_acquire) >= 0 || stop_.load(std::memory_order_acquire)) return;
    int frames = count / 2;
    int rate = rate_.load(std::memory_order_acquire);
    if (drop_until_us_ >= 0 && rate > 0) {
        // A frame with no timestamp used to cancel the drop and play the keyframe
        // while the clock already said the seek target.
        if (frame_us < 0) {
            if (++drop_blind_ < 48) return;
            drop_until_us_ = -1;
            drop_blind_ = 0;
        } else {
            drop_blind_ = 0;
            int64_t dur = audio_frames_us(frames, rate);
            if (frame_us + dur <= drop_until_us_) return;
            if (frame_us < drop_until_us_) {
                int64_t skip64 = ((drop_until_us_ - frame_us) * (int64_t)rate + 999999) / 1000000;
                if (skip64 < 0) skip64 = 0;
                if (skip64 > frames) skip64 = frames;
                src += (int)skip64 * 2;
                frames -= (int)skip64;
                frame_us += audio_frames_us(skip64, rate);
            }
            drop_until_us_ = -1;
        }
    }
    if (frames <= 0) return;
    if (pcm_origin_us_ < 0) pcm_origin_us_ = frame_us >= 0 ? frame_us : snapshot(0, false).position_us;
    int off = 0;
    while (off < frames && !stop_.load(std::memory_order_acquire)) {
        if (seek_ms_.load(std::memory_order_acquire) >= 0) return;
        if (paused_.load(std::memory_order_acquire)) {
            while (sem_wait(&sem_) != 0 && errno == EINTR) {
            }
            continue;
        }
        if (!started_.load(std::memory_order_acquire) && frame_us >= 0) {
            clock_.access([&](PlaybackClockState& c) { c.origin_us = frame_us; });
        }
        maybe_start();
        lyra::audio::Stream* stream = stream_.load(std::memory_order_acquire);
        if (stream == nullptr) return;
        const int16_t* ptr = src + off * 2;
        int chunk = frames - off;
        int16_t faded[256 * 2];
        int left = fade_left_.load(std::memory_order_relaxed);
        int faded_frames = 0;
        if (left > 0) {
            int total = fade_total_.load(std::memory_order_relaxed);
            if (total < 1) total = left;
            int use = chunk < 256 ? chunk : 256;
            if (use > left) use = left;
            int done = total - left;
            for (int i = 0; i < use; ++i) {
                int gain = done + i;
                if (gain < 0) gain = 0;
                if (gain > total) gain = total;
                faded[i * 2] = (int16_t)((int32_t)ptr[i * 2] * gain / total);
                faded[i * 2 + 1] = (int16_t)((int32_t)ptr[i * 2 + 1] * gain / total);
            }
            ptr = faded;
            chunk = use;
            faded_frames = use;
        } else if (chunk > 4096) {
            chunk = 4096;
        }
        int32_t wrote = lyra::audio::write(stream, ptr, chunk, 20000000LL);
        if (wrote == 0) { refresh_clock(stream); continue; }
        if (wrote < 0) {
            if (paused_.load(std::memory_order_acquire) || stop_.load(std::memory_order_acquire)) {
                timespec delay{0, 2000000};
                nanosleep(&delay, nullptr);
                continue;
            }
            if (wrote == lyra::audio::ErrorInvalidState) {
                std::lock_guard<std::recursive_mutex> control(stream_mu_);
                if (paused_.load(std::memory_order_acquire) || stop_.load(std::memory_order_acquire) ||
                    (settle_stream(stream) && lyra::audio::state(stream) == lyra::audio::Started)) continue;
            }
            on_error(wrote);
            return;
        }
        if (faded_frames > 0) {
            int remain = left - wrote;
            if (remain < 0) remain = 0;
            fade_left_.store(remain, std::memory_order_relaxed);
        }
        // Keep both PCM media PTS and the output-frame mapping. A flush may advance
        // output counters; sampling must use the current segment's presentation clock.
        int64_t output_end = lyra::audio::written(stream);
        int64_t media_us = pcm_origin_us_ + audio_frames_us(pcm_frames_, rate);
        features_.feed(ptr, wrote, pcm_origin_us_ * rate / 1000000 + pcm_frames_, output_end-wrote);
        pcm_frames_ += wrote;
        off += wrote;

        int64_t presented = 0, when = 0;
        bool timestamp = lyra::audio::timestamp(stream, &presented, &when) == lyra::audio::Ok && presented >= 0;
        int64_t queued = lyra::audio::written(stream);
        {
            clock_.access([&](PlaybackClockState& c) {
                c.pcm.append(output_end - wrote, wrote, media_us, rate);
                c.publish(presented, when, queued, timestamp);
            });
        }
        int xruns = lyra::audio::xruns(stream);
        if (xruns > last_xruns_) underruns_.fetch_add(xruns - last_xruns_, std::memory_order_relaxed);
        last_xruns_ = xruns;
        if (!paused_.load(std::memory_order_acquire)) { memory_->update(raw_position_ms()); if(listener_)listener_(listener_user_,2,clock_.access([](PlaybackClockState& c){return c.generation;})); }
    }
}

void Engine::refresh_clock(lyra::audio::Stream* stream) {
    int64_t frame = 0, stamp = 0;
    bool valid = lyra::audio::timestamp(stream, &frame, &stamp) == lyra::audio::Ok;
    int64_t queued = lyra::audio::written(stream);
    clock_.access([&](PlaybackClockState& c) { c.publish(frame, stamp, queued, valid); });
}

void Engine::wait_for_drain() {
    lyra::audio::Stream* stream = stream_.load(std::memory_order_acquire);
    if (stream == nullptr) return;
    int64_t written = lyra::audio::written(stream);
    // A pause in the queued tail is still a pause, not an end-of-track timeout.
    int attempts = 0;
    while (attempts < 250 && !stop_.load(std::memory_order_acquire)) {
        if (seek_ms_.load(std::memory_order_acquire) >= 0) return;
        if (!paused_.load(std::memory_order_acquire)) {
            ++attempts;
            int64_t presented = 0, when = 0;
            if (lyra::audio::timestamp(stream, &presented, &when) == lyra::audio::Ok) {
                clock_.access([&](PlaybackClockState& c) {
                    c.publish(presented, when, written, true);
                });
                memory_->update(raw_position_ms());
                if (presented >= written) return;
            }
        }
        timespec delay{0, 20000000}; nanosleep(&delay, nullptr);
    }
}

void Engine::maybe_start() {
    std::lock_guard<std::recursive_mutex> control(stream_mu_);
    if (paused_.load(std::memory_order_acquire) || stop_.load(std::memory_order_acquire) ||
        seek_ms_.load(std::memory_order_acquire) >= 0) return;
    lyra::audio::Stream* stream = stream_.load(std::memory_order_acquire);
    if (stream == nullptr) return;
    int rate = rate_.load(std::memory_order_acquire);
    if (rate < 1) return;
    bool idle = false;
    if (!started_.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) return;
    int ramp = rate / 125;
    if (ramp < 32) ramp = 32;
    fade_total_.store(ramp, std::memory_order_relaxed);
    fade_left_.store(ramp, std::memory_order_release);
    timespec start_time{}; clock_gettime(CLOCK_MONOTONIC, &start_time);
    int64_t base = std::max<int64_t>(0, lyra::audio::written(stream));
    clock_.access([&](PlaybackClockState& c) {
        c.origin_frame = base;
        c.restart(base, start_time.tv_sec*1000000000LL+start_time.tv_nsec);
    });
    lyra::audio::Result rc = lyra::audio::start(stream);
    if (rc != lyra::audio::Ok) {
        started_.store(false, std::memory_order_release);
        on_error(rc);
        return;
    }
    lyra_log(LyraLogInfo, "Lyra", "play start rate=%d ms=%lld", rate,
                        (long long)(now_ms() - decode_mark_.load(std::memory_order_relaxed)));
}

void Engine::on_error(lyra::audio::Result error) {
    lyra_log(LyraLogInfo, "Lyra", "play stream error %d", (int)error);
    if (error == lyra::audio::ErrorDisconnected && !stop_.load(std::memory_order_acquire)) {
        bool recover = clock_.access([&](PlaybackClockState& c) {
            if (!c.active) return false;
            if (recover_output_.exchange(true, std::memory_order_acq_rel)) return true;
            auto audible = c.sample(now_ns());
            c.origin_us = c.frozen_us = audible.position_us;
            c.origin_frame = c.frozen_frame = audible.output_frame;
            c.ready = false; c.timestamp_valid = false; c.pcm.reset(); c.seeking = true;
            if (seek_ms_.load(std::memory_order_acquire) < 0) {
                ++c.seek_serial;
                seek_origin_ = kRecoverySeek;
                seek_ms_.store(audible.position_us / 1000, std::memory_order_release);
            }
            return true;
        });
        if (recover) { sem_post(&sem_); return; }
    }
    fault_.store(true, std::memory_order_release);
    stop_.store(true, std::memory_order_release);
    clock_.access([&](PlaybackClockState& c) {
        c.freeze(now_ns());
        c.active = false; c.seeking = false;
    });
    sem_post(&sem_);
}

void Engine::decode_main() {
    arm_log();
    decode_mark_.store(now_ms(), std::memory_order_relaxed);
    if (setpriority(PRIO_PROCESS, 0, -16) != 0) {
        lyra_log(LyraLogInfo, "Lyra", "play priority errno=%d", errno);
    }
    bool ok = false;
    {
        DecodeSession session;
        session.fd = fd_;
        fd_ = -1;
        uint8_t* buffer = static_cast<uint8_t*>(av_malloc(32768));
        if (buffer == nullptr) {
            log_fail("io", AVERROR(ENOMEM));
        } else {
            session.avio = avio_alloc_context(buffer, 32768, 0, &session, io_read, nullptr, io_seek);
            if (session.avio == nullptr) {
                av_free(buffer);
                log_fail("io", AVERROR(ENOMEM));
            } else {
                session.fmt = avformat_alloc_context();
                if (session.fmt == nullptr) {
                    log_fail("format", AVERROR(ENOMEM));
                } else {
                    session.fmt->pb = session.avio;
                    session.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
                    session.fmt->probesize = 256 * 1024;
                    session.fmt->format_probesize = 256 * 1024;
                    int opened = 0;
                    int info = 0;
                    {
                        std::lock_guard<std::mutex> open_lock(g_ffmpeg_open);
                        opened = avformat_open_input(&session.fmt, nullptr, nullptr, nullptr);
                        info = opened < 0 ? opened : avformat_find_stream_info(session.fmt, nullptr);
                    }
                    if (opened < 0) {
                        log_fail("open", opened);
                    } else if (info < 0) {
                        log_fail("info", info);
                    } else {
                        int index = av_find_best_stream(session.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
                        if (index < 0) {
                            log_fail("stream", index);
                        } else {
                            AVStream* stream = session.fmt->streams[index];
                            const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
                            if (codec == nullptr) {
                                log_fail("codec", AVERROR_DECODER_NOT_FOUND);
                            } else {
                                session.ctx = avcodec_alloc_context3(codec);
                                if (session.ctx == nullptr ||
                                    avcodec_parameters_to_context(session.ctx, stream->codecpar) < 0) {
                                    log_fail("codec", AVERROR(ENOMEM));
                                } else {
                                    session.ctx->pkt_timebase = stream->time_base;
                                    session.ctx->thread_count = 1;
                                    int open_rc = avcodec_open2(session.ctx, codec, nullptr);
                                    if (open_rc < 0) {
                                        log_fail("codec", open_rc);
                                    } else {
                                        lyra_log(LyraLogInfo, "Lyra", "play open %s %s %dhz ch=%d",
                                                            session.fmt->iformat != nullptr ? session.fmt->iformat->name : "?",
                                                            avcodec_get_name(session.ctx->codec_id), session.ctx->sample_rate,
                                                            session.ctx->ch_layout.nb_channels);
                                        session.pkt = av_packet_alloc();
                                        session.frame = av_frame_alloc();
                                        if (session.pkt == nullptr || session.frame == nullptr) {
                                            log_fail("packet", AVERROR(ENOMEM));
                                        } else if (!open_output(session.ctx->sample_rate > 0 ? session.ctx->sample_rate : 48000)) {
                                        } else {
                                            ok = true;
                                            auto apply_seek = [&]() {
                                                uint64_t serial = 0;
                                                SeekOrigin origin = kTransportSeek;
                                                int64_t ms = clock_.access([&](PlaybackClockState& c) {
                                                    serial = c.seek_serial;
                                                    origin = seek_origin_;
                                                    return seek_ms_.exchange(-1, std::memory_order_acq_rel);
                                                });
                                                if (ms < 0) return;
                                                int64_t target = av_rescale_q(ms, AVRational{1, 1000}, stream->time_base);
                                                if (stream->start_time != AV_NOPTS_VALUE) target += stream->start_time;
                                                int rc = av_seek_frame(session.fmt, index, target, AVSEEK_FLAG_BACKWARD);
                                                if (rc < 0) {
                                                    clock_.access([&](PlaybackClockState& c) {
                                                        if (c.seek_serial == serial) {
                                                            c.seeking = false;
                                                            seek_failed_.store(true, std::memory_order_release);
                                                        }
                                                    });
                                                    if (recover_output_.load(std::memory_order_acquire)) on_error(lyra::audio::ErrorInternal);
                                                    lyra_log(LyraLogInfo, "Lyra", "play seek fail %d", rc);
                                                    return;
                                                }
                                                avcodec_flush_buffers(session.ctx);
                                                if (session.swr != nullptr) swr_free(&session.swr);
                                                session.swr_rate = -1;
                                                session.swr_fmt = -1;
                                                session.swr_ch = -1;
                                                std::lock_guard<std::recursive_mutex> control(stream_mu_);
                                                lyra::audio::Stream* audio = stream_.load(std::memory_order_acquire);
                                                if (audio != nullptr && (recover_output_.load(std::memory_order_acquire) || !discard_pending(audio))) {
                                                    // Never map a new lyric time onto the old queued audio.
                                                    close_stream();
                                                    started_.store(false, std::memory_order_release);
                                                    if (!open_output(session.ctx->sample_rate > 0 ? session.ctx->sample_rate : 48000)) {
                                                        fault_.store(true, std::memory_order_release);
                                                        stop_.store(true, std::memory_order_release);
                                                        return;
                                                    }
                                                    audio = stream_.load(std::memory_order_acquire);
                                                }
                                                recover_output_.store(false, std::memory_order_release);
                                                started_.store(false, std::memory_order_release);
                                                int64_t base = audio != nullptr ? std::max<int64_t>(0, lyra::audio::written(audio)) : 0;
                                                clock_.access([&](PlaybackClockState& c) {
                                                    features_.reset(c.rate, feature_settings_);
                                                    c.feature_epoch = features_.epoch();
                                                    c.origin_us = ms * 1000; c.origin_frame = base;
                                                    c.presented = c.submitted = c.frozen_frame = c.restart_frame = base;
                                                    c.stamp_ns = c.restart_ns = 0; c.ready = c.timestamp_valid = false;
                                                    c.sampled_frame = base;
                                                    c.pcm.reset();
                                                    ++c.revision; c.landed_seek = serial;
                                                    c.seeking = c.seek_serial != serial;
                                                });
                                                pcm_origin_us_ = -1; pcm_frames_ = 0;
                                                memory_->update(ms);
                                                memory_->checkpoint();
                                                drop_until_us_ = ms * 1000;
                                                drop_blind_ = 0;
                                                lyra_log(LyraLogInfo, "Lyra", "play seek ms=%lld origin=%d serial=%llu",
                                                    (long long)ms, int(origin), (unsigned long long)serial);
                                            };
                                            publish_lyrics(lyrics_from_context(session.fmt));
                                        decode_packets:
                                            while (!stop_.load(std::memory_order_acquire)) {
                                                if (seek_ms_.load(std::memory_order_acquire) >= 0) {
                                                    apply_seek();
                                                    continue;
                                                }
                                                int read_rc = av_read_frame(session.fmt, session.pkt);
                                                if (read_rc == AVERROR_EOF) break;
                                                if (read_rc < 0) {
                                                    log_fail("read", read_rc);
                                                    ok = false;
                                                    break;
                                                }
                                                if (session.pkt->stream_index == index) {
                                                    int send = avcodec_send_packet(session.ctx, session.pkt);
                                                    if (send < 0 && send != AVERROR(EAGAIN)) {
                                                        log_fail("send", send);
                                                        ok = false;
                                                        av_packet_unref(session.pkt);
                                                        break;
                                                    }
                                                    for (;;) {
                                                        int recv = avcodec_receive_frame(session.ctx, session.frame);
                                                        if (recv == AVERROR(EAGAIN) || recv == AVERROR_EOF) break;
                                                        if (recv < 0) {
                                                            log_fail("decode", recv);
                                                            ok = false;
                                                            break;
                                                        }
                                                        if (!stop_.load(std::memory_order_acquire)) {
                                                            int out_rate = rate_.load(std::memory_order_acquire);
                                                            bool pass = session.frame->format == AV_SAMPLE_FMT_S16 &&
                                                                        session.frame->sample_rate == out_rate &&
                                                                        session.frame->ch_layout.nb_channels == 2 &&
                                                                        session.frame->data[0] != nullptr;
                                                            if (pass) {
                                                                feed(reinterpret_cast<int16_t*>(session.frame->data[0]),
                                                                     session.frame->nb_samples * 2, frame_media_us(session.frame, stream));
                                                            } else if (out_rate > 0 && session.frame->nb_samples > 0) {
                                                                bool same = session.swr != nullptr && session.swr_rate == session.frame->sample_rate &&
                                                                            session.swr_fmt == session.frame->format &&
                                                                            session.swr_ch == session.frame->ch_layout.nb_channels;
                                                                if (!same) {
                                                                    if (session.swr != nullptr) swr_free(&session.swr);
                                                                    AVChannelLayout out_layout;
                                                                    av_channel_layout_default(&out_layout, 2);
                                                                    int swr_rc = swr_alloc_set_opts2(
                                                                        &session.swr, &out_layout, AV_SAMPLE_FMT_S16, out_rate,
                                                                        &session.frame->ch_layout, (AVSampleFormat)session.frame->format,
                                                                        session.frame->sample_rate, 0, nullptr);
                                                                    av_channel_layout_uninit(&out_layout);
                                                                    if (swr_rc < 0 || session.swr == nullptr || swr_init(session.swr) < 0) {
                                                                        log_fail("resample", swr_rc < 0 ? swr_rc : AVERROR_UNKNOWN);
                                                                        ok = false;
                                                                    } else {
                                                                        session.swr_rate = session.frame->sample_rate;
                                                                        session.swr_fmt = session.frame->format;
                                                                        session.swr_ch = session.frame->ch_layout.nb_channels;
                                                                    }
                                                                }
                                                                if (ok && session.swr != nullptr) {
                                                                    int out_count = swr_get_out_samples(session.swr, session.frame->nb_samples);
                                                                    if (out_count < session.frame->nb_samples) out_count = session.frame->nb_samples;
                                                                    if (out_count < 1) out_count = 1;
                                                                    if (out_count > (1 << 20)) out_count = 1 << 20;
                                                                    session.pcm.resize((size_t)out_count * 2);
                                                                    uint8_t* out_planes[] = {reinterpret_cast<uint8_t*>(session.pcm.data())};
                                                                    int64_t media_us = frame_media_us(session.frame, stream);
                                                                    int input_rate = session.frame->sample_rate;
                                                                    if (media_us >= 0 && input_rate > 0) media_us = std::max<int64_t>(0, media_us -
                                                                        av_rescale(swr_get_delay(session.swr, input_rate), 1000000, input_rate));
                                                                    int got = swr_convert(session.swr, out_planes, out_count,
                                                                                          (const uint8_t**)session.frame->extended_data,
                                                                                          session.frame->nb_samples);
                                                                    if (got < 0) {
                                                                        log_fail("resample", got);
                                                                        ok = false;
                                                                    } else if (got > 0) {
                                                                        feed(session.pcm.data(), got * 2, media_us);
                                                                    }
                                                                }
                                                            }
                                                        }
                                                        av_frame_unref(session.frame);
                                                        if (!ok) break;
                                                    }
                                                }
                                                av_packet_unref(session.pkt);
                                                if (!ok) break;
                                            }
                                            if (ok && !stop_.load(std::memory_order_acquire)) {
                                                avcodec_send_packet(session.ctx, nullptr);
                                                for (;;) {
                                                    int recv = avcodec_receive_frame(session.ctx, session.frame);
                                                    if (recv == AVERROR(EAGAIN) || recv == AVERROR_EOF) break;
                                                    if (recv < 0) {
                                                        ok = false;
                                                        break;
                                                    }
                                                    int out_rate = rate_.load(std::memory_order_acquire);
                                                    bool pass = session.frame->format == AV_SAMPLE_FMT_S16 &&
                                                                session.frame->sample_rate == out_rate &&
                                                                session.frame->ch_layout.nb_channels == 2 &&
                                                                session.frame->data[0] != nullptr;
                                                    if (pass) {
                                                        feed(reinterpret_cast<int16_t*>(session.frame->data[0]),
                                                             session.frame->nb_samples * 2, frame_media_us(session.frame, stream));
                                                    } else if (session.swr != nullptr) {
                                                        int out_count = swr_get_out_samples(session.swr, session.frame->nb_samples);
                                                        if (out_count < 1) out_count = 1;
                                                        if (out_count > (1 << 20)) out_count = 1 << 20;
                                                        session.pcm.resize((size_t)out_count * 2);
                                                        uint8_t* out_planes[] = {reinterpret_cast<uint8_t*>(session.pcm.data())};
                                                        int64_t media_us = frame_media_us(session.frame, stream);
                                                        int input_rate = session.frame->sample_rate;
                                                        if (media_us >= 0 && input_rate > 0) media_us = std::max<int64_t>(0, media_us -
                                                            av_rescale(swr_get_delay(session.swr, input_rate), 1000000, input_rate));
                                                        int got = swr_convert(session.swr, out_planes, out_count,
                                                                              (const uint8_t**)session.frame->extended_data,
                                                                              session.frame->nb_samples);
                                                        if (got > 0) feed(session.pcm.data(), got * 2, media_us);
                                                    }
                                                    av_frame_unref(session.frame);
                                                }
                                                if (session.swr != nullptr && ok) {
                                                    int out_rate = rate_.load(std::memory_order_acquire);
                                                    int cap = out_rate > 0 ? out_rate : 48000;
                                                    session.pcm.resize((size_t)cap * 2);
                                                    uint8_t* out_planes[] = {reinterpret_cast<uint8_t*>(session.pcm.data())};
                                                    for (;;) {
                                                        int got = swr_convert(session.swr, out_planes, cap, nullptr, 0);
                                                        if (got <= 0) break;
                                                        feed(session.pcm.data(), got * 2, -1);
                                                    }
                                                }
                                            }
                                            if (ok && !fault_.load(std::memory_order_acquire) &&
                                                exit_.load(std::memory_order_acquire) != kUser) {
                                                maybe_start();
                                                wait_for_drain();
                                                if (!stop_.load(std::memory_order_acquire) && seek_ms_.load(std::memory_order_acquire) >= 0) goto decode_packets;
                                                if (!stop_.load(std::memory_order_acquire)) {
                                                    memory_->update(0);
                                                    memory_->checkpoint();
                                                }
                                                lyra_log(LyraLogInfo, "Lyra", "play end");
                                            } else {
                                                ok = false;
                                            }
                                            if (exit_.load(std::memory_order_acquire) != kUser ||
                                                !keep_stream_.load(std::memory_order_acquire)) {
                                                close_stream();
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if (fault_.load(std::memory_order_acquire)) ok = false;
    if (stop_.load(std::memory_order_acquire) && exit_.load(std::memory_order_acquire) == kUser) ok = false;
    uint64_t generation = clock_.access([&](PlaybackClockState& c) {
        c.freeze(now_ns());
        c.active = false; c.seeking = false;
        return c.generation;
    });
    finished_.store(true, std::memory_order_release);
    if (exit_.load(std::memory_order_acquire) != kUser && listener_ != nullptr) listener_(listener_user_, ok ? kEnded : kFailed, generation);
}
