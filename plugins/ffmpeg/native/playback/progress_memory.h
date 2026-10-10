#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <pthread.h>
#include <string>

struct PlaybackBookmark {
    int64_t track = -1, position = 0, duration = 0;
};
// Disk writes belong to this worker, never the UI or the audio writer.
class ProgressMemory {
  public:
    ~ProgressMemory();
    PlaybackBookmark configure(const std::string &path, int interval_ms);
    void select(int64_t track, int64_t position, int64_t duration);
    void update(int64_t position);
    void checkpoint();
    void flush();
    static PlaybackBookmark read(const std::string &path);

  private:
    static void *entry(void *self);
    void run();
    bool write(const PlaybackBookmark &mark);
    std::string path_;
    std::mutex mutex_;
    std::condition_variable wake_, saved_;
    pthread_t thread_{};
    PlaybackBookmark current_;
    std::atomic<int64_t> position_{0};
    uint64_t revision_ = 0, saved_revision_ = 0;
    int interval_ms_ = 1000;
    bool live_ = false, stop_ = false;
};
