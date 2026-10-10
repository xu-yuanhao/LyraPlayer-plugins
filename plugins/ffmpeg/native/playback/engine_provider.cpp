#include "playback/engine.h"
#include "playback/provider.h"
#include <cstring>
struct EngineBackend {
    Engine engine;
    LyraPlaybackEventFn listener = nullptr;
    void *listener_user = nullptr;
    std::string lyric;
    uint64_t lyric_generation = 0;
    int64_t lyric_track = -1;
};
OwnedPlaybackBackend make_engine_backend() {
    auto *backend = new EngineBackend;
    OwnedPlaybackBackend out;
    auto &api = out.api;
    api.size = sizeof(api);
    api.version = LYRA_INTERFACE_VERSION;
    api.user = backend;
    api.snapshot = [](void *user, int64_t target, int32_t features, LyraPlaybackSnapshotV2 *result) {
        if (!result || result->size < sizeof(*result))
            return -1;
        *result = playback_export(static_cast<EngineBackend *>(user)->engine.snapshot(target, features));
        return 0;
    };
    api.command = [](void *user, const LyraPlaybackCommandV2 *c) {
        if (!c || c->size < sizeof(*c))
            return -1;
        auto &engine = static_cast<EngineBackend *>(user)->engine;
        switch (c->kind) {
        case LYRA_PLAYBACK_OPEN:
            engine.prime_start(c->position_us / 1000, c->flags & LYRA_PLAYBACK_PAUSED);
            engine.start(c->fd, c->track_id, c->duration_us / 1000, {c->bass_hz, c->wide_hz, c->hop_ms, c->accent_gain},
                         c->generation, {c->capacity_ms, c->bursts});
            break;
        case LYRA_PLAYBACK_STOP:
            engine.stop();
            break;
        case LYRA_PLAYBACK_PAUSE:
            engine.pause();
            break;
        case LYRA_PLAYBACK_RESUME:
            return engine.resume() ? 0 : -1;
        case LYRA_PLAYBACK_SEEK:
            engine.seek(c->position_us / 1000, Engine::SeekOrigin(c->seek_origin));
            break;
        case LYRA_PLAYBACK_PRIME:
            engine.prime_start(c->position_us / 1000, c->flags & LYRA_PLAYBACK_PAUSED);
            break;
        case LYRA_PLAYBACK_REMEMBER:
            engine.remember_selection(c->track_id, c->position_us / 1000, c->duration_us / 1000, c->generation);
            break;
        case LYRA_PLAYBACK_CLEAR_PRIME:
            engine.clear_prime();
            break;
        case LYRA_PLAYBACK_REAP:
            engine.reap();
            break;
        default:
            return -1;
        }
        return 0;
    };
    api.quiesce = [](void *user) {
        static_cast<EngineBackend *>(user)->engine.stop();
        return 1;
    };
    api.listener = [](void *user, LyraPlaybackEventFn fn, void *target) {
        auto &b = *static_cast<EngineBackend *>(user);
        b.listener = fn;
        b.listener_user = target;
        b.engine.set_listener(
            [](void *owner, int event, uint64_t generation) {
                auto &b = *static_cast<EngineBackend *>(owner);
                if (b.listener) { auto progress=playback_export(b.engine.snapshot(0,false)); b.listener(b.listener_user,event,generation,event==2?&progress:nullptr); }
            },
            user);
    };
    api.lyrics = [](void *user, uint64_t generation, int64_t track, char *buffer, int capacity) {
        auto &b = *static_cast<EngineBackend *>(user);
        if (b.lyric_generation != generation || b.lyric_track != track) {
            b.lyric.clear();
            b.lyric_generation = generation;
            b.lyric_track = track;
        }
        if (b.lyric.empty())
            b.engine.take_lyrics(&b.lyric, generation, track);
        int length = int(b.lyric.size());
        if (!buffer || capacity <= 0)
            return length;
        if (capacity <= length)
            return -1;
        std::memcpy(buffer, b.lyric.data(), size_t(length));
        buffer[length] = 0;
        b.lyric.clear();
        return length;
    };
    out.destroy = [](void *user) { delete static_cast<EngineBackend *>(user); };
    out.attach_memory = [](void *user, ProgressMemory *memory) {
        static_cast<EngineBackend *>(user)->engine.set_memory(memory);
    };
    out.seek_failed = [](void *user) { return static_cast<EngineBackend *>(user)->engine.take_seek_failed(); };
    out.underruns = [](void *user) { static_cast<EngineBackend *>(user)->engine.log_underruns(); };
    return out;
}
