#include "playback/progress_memory.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

PlaybackBookmark ProgressMemory::read(const std::string &path) {
    PlaybackBookmark out;
    FILE *file = std::fopen(path.c_str(), "r");
    if (!file)
        return out;
    long long id = -1, position = 0, duration = 0;
    char tail = 0;
    int count = std::fscanf(file, "LYRA1 %lld %lld %lld %c", &id, &position, &duration, &tail);
    std::fclose(file);
    if (count != 3 || id <= 0 || position < 0 || duration < 0 || (duration > 0 && position > duration))
        return out;
    out.track = id;
    out.position = position;
    out.duration = duration;
    return out;
}

PlaybackBookmark ProgressMemory::configure(const std::string &path, int interval_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (live_ || path.empty())
        return current_;
    path_ = path;
    interval_ms_ = std::max(250, interval_ms);
    current_ = read(path);
    position_.store(current_.position);
    live_ = pthread_create(&thread_, nullptr, entry, this) == 0;
    return current_;
}

ProgressMemory::~ProgressMemory() {
    if (!live_)
        return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        ++revision_;
    }
    wake_.notify_one();
    pthread_join(thread_, nullptr);
}

void ProgressMemory::select(int64_t track, int64_t position, int64_t duration) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = {track, std::max<int64_t>(0, position), std::max<int64_t>(0, duration)};
        position_.store(current_.position);
        ++revision_;
    }
    wake_.notify_one();
}
void ProgressMemory::update(int64_t position) {
    position_.store(std::max<int64_t>(0, position));
}
void ProgressMemory::checkpoint() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++revision_;
    }
    wake_.notify_one();
}
void ProgressMemory::flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!live_)
        return;
    uint64_t rev = ++revision_;
    wake_.notify_one();
    saved_.wait(lock, [&] { return saved_revision_ >= rev; });
}
void *ProgressMemory::entry(void *self) {
    static_cast<ProgressMemory *>(self)->run();
    return nullptr;
}

bool ProgressMemory::write(const PlaybackBookmark &mark) {
    if (mark.track <= 0)
        return true;
    std::string temp = path_ + ".tmp";
    FILE *file = std::fopen(temp.c_str(), "w");
    if (!file)
        return false;
    bool ok = std::fprintf(file, "LYRA1 %lld %lld %lld\n", (long long)mark.track, (long long)mark.position,
                           (long long)mark.duration) > 0;
    ok = std::fflush(file) == 0 && ok;
    ok = fsync(fileno(file)) == 0 && ok;
    ok = std::fclose(file) == 0 && ok;
    if (!ok || rename(temp.c_str(), path_.c_str()) != 0) {
        unlink(temp.c_str());
        return false;
    }
    size_t slash = path_.rfind('/');
    int dir = open(path_.substr(0, slash).c_str(), O_RDONLY | O_DIRECTORY);
    if (dir >= 0) {
        fsync(dir);
        close(dir);
    }
    return true;
}
void ProgressMemory::run() {
    pthread_setname_np(pthread_self(), "LyraProgress");
    setpriority(PRIO_PROCESS, 0, 10);
    PlaybackBookmark last;
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait_for(lock, std::chrono::milliseconds(interval_ms_),
                       [&] { return stop_ || revision_ > saved_revision_; });
        PlaybackBookmark next = current_;
        next.position = position_.load();
        if (next.duration > 0)
            next.position = std::min(next.position, next.duration);
        uint64_t rev = revision_;
        bool done = stop_;
        lock.unlock();
        bool ok = next.track == last.track && next.position == last.position && next.duration == last.duration;
        if (!ok)
            ok = write(next);
        if (ok)
            last = next;
        lock.lock();
        saved_revision_ = rev;
        saved_.notify_all();
        if (done)
            return;
    }
}
