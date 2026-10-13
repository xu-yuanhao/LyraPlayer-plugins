// Netease free-music media provider (official.netease, ABI 3).
//
// A pure LyraMediaProviderV3: it answers SEARCH/AUDIO/COVER/LYRICS from the
// public Netease endpoints and never replaces a built-in service. Search
// results carry only free tracks (fee == 0); VIP/paid tracks are skipped at
// parse time, so they never reach the host. Local results keep their order:
// the host appends provider rows after local matches.
//
// Networking runs on three worker threads through JNI HttpURLConnection
// (system TLS, no bundled crypto). Only the JavaVM is cached; the Activity
// is used once to resolve the cache dir path and never retained. Audio and
// cover bytes are never proxied: AUDIO/COVER resolve to an https url and the
// host's verified downloader fetches the file. Lyrics are small text, so the
// LYRICS handler writes them to an unlinked temp file and transfers the fd.
#include "plugin/lyra_plugin.h"

#include <fcntl.h>
#include <jni.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "net_http.h"
#include "netease_json.h"

namespace {

const char *kProviderId = "official.netease";
const char *kUidPrefix = "netease:";
const char *kFolder = "网易云音乐";
const size_t kMaxResults = 10;
const size_t kCoverCacheMax = 1024;

struct Task {
    uint32_t kind = 0;
    uint64_t request_id = 0;
    std::string collection;
    std::string query;
    std::string cursor;
    std::string uid;
    LyraMediaSinkV3 sink{};
};

struct PluginState {
    JavaVM *vm = nullptr;
    std::string cache_dir;
    std::mutex mutex;
    std::condition_variable cv;
    std::queue<Task> pending;
    std::unordered_map<uint64_t, bool> cancelled;
    std::unordered_map<std::string, std::string> covers;
    std::vector<std::thread> workers;
    bool stop = false;
};

PluginState g_state;
LyraMediaProviderV3 g_provider{};

std::string StrOrEmpty(const char *s) { return s ? std::string(s) : std::string(); }

bool WasCancelled(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    auto it = g_state.cancelled.find(id);
    return it != g_state.cancelled.end() && it->second;
}

void NoteFinished(uint64_t id) {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.cancelled.erase(id);
    if (g_state.cancelled.size() > 4096) g_state.cancelled.clear();
}

void RememberCover(const std::string &uid, const std::string &cover) {
    if (uid.empty() || cover.empty()) return;
    std::lock_guard<std::mutex> lock(g_state.mutex);
    if (g_state.covers.size() > kCoverCacheMax) g_state.covers.clear();
    g_state.covers[uid] = cover;
}

std::string LookupCover(const std::string &uid) {
    std::lock_guard<std::mutex> lock(g_state.mutex);
    auto it = g_state.covers.find(uid);
    return it != g_state.covers.end() ? it->second : std::string();
}

std::string SongIdFromUid(const std::string &uid) {
    const char *p = kUidPrefix;
    size_t n = std::strlen(p);
    if (uid.compare(0, n, p) != 0) return std::string();
    std::string id = uid.substr(n);
    return netease::IsDigits(id) ? id : std::string();
}

std::string SearchUrl(const std::string &query) {
    return "https://music.163.com/api/cloudsearch/pc?s=" +
           netease::UrlEncode(query) + "&type=1&offset=0&limit=10";
}

std::string OuterUrl(const std::string &song_id) {
    return "https://music.163.com/song/media/outer/url?id=" + song_id + ".mp3";
}

std::string DetailUrl(const std::string &song_id) {
    return "https://music.163.com/api/song/detail?ids=%5B" + song_id + "%5D";
}

std::string LyricUrl(const std::string &song_id) {
    return "https://music.163.com/api/song/lyric?id=" + song_id +
           "&lv=1&kv=1&tv=-1";
}

std::string WithCoverParams(const std::string &cover) {
    if (cover.empty()) return cover;
    if (cover.find('?') != std::string::npos) return cover;
    return cover + "?param=200y200";
}

void PublishEmpty(const Task &task) {
    LyraMediaBatchV3 batch{};
    batch.size = sizeof(batch);
    batch.flags = LYRA_MEDIA_COMPLETE;
    batch.request_id = task.request_id;
    batch.items = nullptr;
    batch.count = 0;
    batch.cursor = "";
    batch.error = 0;
    task.sink.publish(task.sink.user, &batch);
}

void PublishError(const Task &task) {
    LyraMediaBatchV3 batch{};
    batch.size = sizeof(batch);
    batch.flags = LYRA_MEDIA_COMPLETE;
    batch.request_id = task.request_id;
    batch.items = nullptr;
    batch.count = 0;
    batch.cursor = "";
    batch.error = -1;
    task.sink.publish(task.sink.user, &batch);
}

void ResolveError(const Task &task) {
    LyraMediaResourceV3 resource{};
    resource.size = sizeof(resource);
    resource.request_id = task.request_id;
    resource.fd = -1;
    resource.error = -1;
    task.sink.resolved(task.sink.user, &resource);
}

void HandleSearch(const Task &task) {
    if (task.query.empty()) {
        PublishEmpty(task);
        return;
    }
    netease::HttpResponse res;
    if (netease::HttpGet(g_state.vm, SearchUrl(task.query).c_str(), 10000,
                         10000, 262144, false, &res) != 0 ||
        res.body.empty()) {
        PublishError(task);
        return;
    }
    if (WasCancelled(task.request_id)) return;
    std::vector<netease::SongInfo> songs;
    if (netease::ParseSongs(res.body.data(), res.body.size(), songs,
                            kMaxResults) < 0) {
        PublishError(task);
        return;
    }
    if (WasCancelled(task.request_id)) return;
    // Storage for every string a descriptor points at; alive through publish.
    std::vector<std::string> keep;
    keep.reserve(songs.size() * 6 + 1);
    std::vector<LyraMediaDescriptorV3> items;
    items.reserve(songs.size());
    for (const netease::SongInfo &song : songs) {
        keep.push_back(kUidPrefix + song.id);
        keep.push_back(song.title);
        keep.push_back(song.artist);
        keep.push_back(song.album);
        keep.push_back(kFolder);
        std::string cover =
            WithCoverParams(netease::ToHttps126(song.cover));
        keep.push_back(cover);
        size_t base = keep.size() - 6;
        RememberCover(keep[base], keep[base + 5]);
        LyraMediaDescriptorV3 d{};
        d.size = sizeof(d);
        d.uid = keep[base].c_str();
        d.title = keep[base + 1].c_str();
        d.artist = keep[base + 2].c_str();
        d.album = keep[base + 3].c_str();
        d.genre = nullptr;
        d.folder = keep[base + 4].c_str();
        d.audio_uri = nullptr;  // resolved on demand by the AUDIO handler
        d.cover_uri = keep[base + 5].empty() ? nullptr : keep[base + 5].c_str();
        d.lyrics_uri = nullptr;
        d.lyrics_text = nullptr;  // fetched on demand by LYRICS handler
        d.duration_us = song.duration_ms * 1000;
        d.cover_key = 0;  // host falls back to the item id
        d.origin_provider = nullptr;  // filled by the host exchange
        d.origin_uid = nullptr;
        items.push_back(d);
    }
    LyraMediaBatchV3 batch{};
    batch.size = sizeof(batch);
    batch.flags = LYRA_MEDIA_COMPLETE;
    batch.request_id = task.request_id;
    batch.items = items.empty() ? nullptr : items.data();
    batch.count = static_cast<uint32_t>(items.size());
    batch.cursor = "";
    batch.error = 0;
    task.sink.publish(task.sink.user, &batch);
}

void HandleAudio(const Task &task) {
    std::string song_id = SongIdFromUid(task.uid);
    if (song_id.empty()) {
        ResolveError(task);
        return;
    }
    std::string page = OuterUrl(song_id);
    netease::HttpResponse res;
    if (netease::HttpGet(g_state.vm, page.c_str(), 10000, 15000, 16384, true,
                         &res) != 0 ||
        res.final_url.empty() || res.final_url == page) {
        // No redirect to a media file: the page itself is not playable.
        ResolveError(task);
        return;
    }
    if (WasCancelled(task.request_id)) return;
    std::string playable = netease::ToHttps126(res.final_url);
    if (playable.compare(0, 8, "https://") != 0) {
        ResolveError(task);  // host downloads https only
        return;
    }
    LyraMediaResourceV3 resource{};
    resource.size = sizeof(resource);
    resource.request_id = task.request_id;
    resource.fd = -1;
    resource.error = 0;
    resource.uri = playable.c_str();
    resource.headers_json = nullptr;
    resource.mime = "audio/mpeg";
    task.sink.resolved(task.sink.user, &resource);
}

void HandleCover(const Task &task) {
    std::string cover = LookupCover(task.uid);
    if (cover.empty()) {
        std::string song_id = SongIdFromUid(task.uid);
        if (!song_id.empty()) {
            netease::HttpResponse res;
            if (netease::HttpGet(g_state.vm, DetailUrl(song_id).c_str(), 10000,
                                 10000, 65536, false, &res) == 0 &&
                !res.body.empty()) {
                std::vector<netease::SongInfo> songs;
                if (netease::ParseSongs(res.body.data(), res.body.size(),
                                       songs, 1) > 0 &&
                    !songs[0].cover.empty()) {
                    cover = WithCoverParams(
                        netease::ToHttps126(songs[0].cover));
                    RememberCover(task.uid, cover);
                }
            }
        }
    }
    if (WasCancelled(task.request_id)) return;
    if (cover.empty() || cover.compare(0, 8, "https://") != 0) {
        ResolveError(task);
        return;
    }
    LyraMediaResourceV3 resource{};
    resource.size = sizeof(resource);
    resource.request_id = task.request_id;
    resource.fd = -1;
    resource.error = 0;
    resource.uri = cover.c_str();
    resource.headers_json = nullptr;
    resource.mime = nullptr;  // host sniffs the bytes
    task.sink.resolved(task.sink.user, &resource);
}

void HandleLyrics(const Task &task) {
    std::string song_id = SongIdFromUid(task.uid);
    if (song_id.empty()) {
        ResolveError(task);
        return;
    }
    netease::HttpResponse res;
    if (netease::HttpGet(g_state.vm, LyricUrl(song_id).c_str(), 10000, 10000,
                         1048576, false, &res) != 0 ||
        res.body.empty()) {
        ResolveError(task);
        return;
    }
    std::string lrc;
    if (netease::ParseLyric(res.body.data(), res.body.size(), lrc) != 0 ||
        lrc.empty()) {
        ResolveError(task);
        return;
    }
    if (WasCancelled(task.request_id)) return;
    // Unlinked temp file: no storage leak, fd ownership transfers to host.
    std::string pattern = g_state.cache_dir + "/lyra-netease-XXXXXX";
    std::vector<char> path(pattern.begin(), pattern.end());
    path.push_back('\0');
    int fd = mkstemp(path.data());
    if (fd < 0) {
        ResolveError(task);
        return;
    }
    size_t done = 0;
    while (done < lrc.size()) {
        ssize_t n = ::write(fd, lrc.data() + done, lrc.size() - done);
        if (n <= 0) {
            ::close(fd);
            ::unlink(path.data());
            ResolveError(task);
            return;
        }
        done += static_cast<size_t>(n);
    }
    ::lseek(fd, 0, SEEK_SET);
    ::unlink(path.data());
    LyraMediaResourceV3 resource{};
    resource.size = sizeof(resource);
    resource.request_id = task.request_id;
    resource.fd = fd;
    resource.error = 0;
    task.sink.resolved(task.sink.user, &resource);
}

void HandleTask(const Task &task) {
    switch (task.kind) {
        case LYRA_MEDIA_SEARCH:
            HandleSearch(task);
            break;
        case LYRA_MEDIA_COLLECTION:
            PublishEmpty(task);  // no browsable library; search only
            break;
        case LYRA_MEDIA_AUDIO:
            HandleAudio(task);
            break;
        case LYRA_MEDIA_COVER:
            HandleCover(task);
            break;
        case LYRA_MEDIA_LYRICS:
            HandleLyrics(task);
            break;
        default:
            break;  // unknown kinds are ignored, never fail loudly
    }
    NoteFinished(task.request_id);
}

void WorkerMain() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(g_state.mutex);
            g_state.cv.wait(lock, [] {
                return g_state.stop || !g_state.pending.empty();
            });
            if (g_state.stop) return;
            task = g_state.pending.front();
            g_state.pending.pop();
        }
        if (WasCancelled(task.request_id)) {
            NoteFinished(task.request_id);
            continue;
        }
        HandleTask(task);
    }
}

int ProviderRequest(void *user, const LyraMediaRequestV3 *r,
                    const LyraMediaSinkV3 *s) {
    (void)user;
    if (!r || !s || r->size != sizeof(*r) || s->size != sizeof(*s)) return -1;
    if (r->kind != LYRA_MEDIA_SEARCH && r->kind != LYRA_MEDIA_COLLECTION &&
        r->kind != LYRA_MEDIA_AUDIO && r->kind != LYRA_MEDIA_COVER &&
        r->kind != LYRA_MEDIA_LYRICS)
        return -1;
    Task task;
    task.kind = r->kind;
    task.request_id = r->request_id;
    task.collection = StrOrEmpty(r->collection);
    task.query = StrOrEmpty(r->query);
    task.cursor = StrOrEmpty(r->cursor);
    task.uid = StrOrEmpty(r->uid);
    task.sink = *s;  // valid until shutdown returns
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        if (g_state.stop) return -1;
        g_state.cancelled[task.request_id] = false;
        g_state.pending.push(task);
    }
    g_state.cv.notify_one();
    return 0;
}

void ProviderCancel(void *user, uint64_t request_id) {
    (void)user;
    std::lock_guard<std::mutex> lock(g_state.mutex);
    g_state.cancelled[request_id] = true;
}

const void *ProviderQuery(const LyraHostV3 *host, uint32_t id,
                          uint32_t version, uint32_t size) {
    (void)host;
    if (id == LYRA_INTERFACE_MEDIA_PROVIDER &&
        version == LYRA_INTERFACE_VERSION && size == sizeof(g_provider))
        return &g_provider;
    return nullptr;
}

// Resolve the private cache dir path once. The Activity object itself is
// never retained: it may be recreated while the plugin lives.
std::string QueryCacheDir(JavaVM *vm, void *android_context) {
    if (!vm || !android_context) return std::string();
    JNIEnv *env = nullptr;
    bool attached = false;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) ==
        JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != 0 || !env)
            return std::string();
        attached = true;
    }
    std::string out;
    jobject ctx = static_cast<jobject>(android_context);
    jclass ctx_cls = env->GetObjectClass(ctx);
    if (env->ExceptionCheck() || !ctx_cls) goto done;
    {
        jmethodID get_cache = env->GetMethodID(ctx_cls, "getCacheDir",
                                               "()Ljava/io/File;");
        if (env->ExceptionCheck() || !get_cache) goto done;
        jobject dir = env->CallObjectMethod(ctx, get_cache);
        if (env->ExceptionCheck() || !dir) goto done;
        jclass file_cls = env->GetObjectClass(dir);
        if (env->ExceptionCheck() || !file_cls) {
            env->DeleteLocalRef(dir);
            goto done;
        }
        jmethodID get_path = env->GetMethodID(file_cls, "getAbsolutePath",
                                               "()Ljava/lang/String;");
        if (env->ExceptionCheck() || !get_path) {
            env->DeleteLocalRef(dir);
            env->DeleteLocalRef(file_cls);
            goto done;
        }
        jstring path =
            static_cast<jstring>(env->CallObjectMethod(dir, get_path));
        if (!env->ExceptionCheck() && path) {
            const char *chars = env->GetStringUTFChars(path, nullptr);
            if (!env->ExceptionCheck() && chars) {
                out = chars;
                env->ReleaseStringUTFChars(path, chars);
            }
            env->DeleteLocalRef(path);
        }
        env->DeleteLocalRef(dir);
        env->DeleteLocalRef(file_cls);
        env->DeleteLocalRef(ctx_cls);
    }
done:
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) vm->DetachCurrentThread();
    return out;
}

int PluginInit(const LyraHostV3 *host) {
    if (!host || !host->java_vm) return -1;
    g_state.vm = static_cast<JavaVM *>(host->java_vm);
    std::string dir =
        QueryCacheDir(g_state.vm, host->android_context);
    if (dir.empty()) {
        const char *tmp = std::getenv("TMPDIR");
        dir = tmp && *tmp ? tmp : "/data/local/tmp";
    }
    g_state.cache_dir = dir;
    g_provider.size = sizeof(g_provider);
    g_provider.version = LYRA_INTERFACE_VERSION;
    g_provider.user = nullptr;
    g_provider.id = kProviderId;
    g_provider.request = ProviderRequest;
    g_provider.cancel = ProviderCancel;
    for (int i = 0; i < 3; ++i) g_state.workers.emplace_back(WorkerMain);
    return 0;
}

void PluginShutdown(const LyraHostV3 *host) {
    (void)host;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        g_state.stop = true;
        // Drop queued work; in-flight workers finish then exit.
        while (!g_state.pending.empty()) g_state.pending.pop();
    }
    g_state.cv.notify_all();
    for (std::thread &t : g_state.workers) {
        if (t.joinable()) t.join();
    }
    g_state.workers.clear();
}

}  // namespace

extern "C" __attribute__((visibility("default"))) int lyra_plugin_init(
    const LyraHostV3 *host, LyraPluginV3 *out) {
    if (!host || host->abi != LYRA_PLUGIN_ABI || !out) return -1;
    *out = LyraPluginV3{};
    out->size = sizeof(*out);
    out->abi = LYRA_PLUGIN_ABI;
    out->id = "official.netease";
    out->kind = "media-data";
    out->version = LYRA_PLUGIN_VERSION;
    out->config_tables = nullptr;
    out->init = PluginInit;
    out->shutdown = PluginShutdown;
    out->query_interface = ProviderQuery;
    return 0;
}
