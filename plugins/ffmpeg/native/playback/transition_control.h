#pragma once
#include "playback/transition_settings.h"
#include "plugin/transition_contracts.h"
#include "plugin/stem_contracts.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <algorithm>
#include <string>
class AudioTransitionControl {
public:
    struct AnalysisJob {int fd=-1;int64_t id=-1,duration_us=0;};
    AudioTransitionControl();
    ~AudioTransitionControl();
    const LyraStemAnalysisV3 *stem_endpoint()const{std::lock_guard<std::mutex> lock(mutex_);return stem_;}
    std::atomic<bool> analysis_busy{false};
    void enqueue_analysis(int fd,int64_t id,int64_t duration_us);
    bool pop_analysis(AnalysisJob *job);
    void wait_analysis(const std::atomic<bool> &stop,uint64_t *revision);
    void wake_analysis(){changed_.notify_all();}

    const LyraAudioTransitionV3 *interface()const{return &api_;}
    AudioTransitionSettings settings()const{std::lock_guard<std::mutex> lock(mutex_);return settings_;}
    std::string cache_directory()const{std::lock_guard<std::mutex> lock(mutex_);return cache_directory_;}
    void publish_edges(uint64_t generation,int64_t end_us){std::lock_guard<std::mutex> lock(mutex_);edge_generation_=generation;edge_end_=end_us;}
    int64_t audible_end(uint64_t generation,int64_t fallback)const{std::lock_guard<std::mutex> lock(mutex_);return settings_.edges.enabled && edge_generation_==generation && edge_end_>0?std::min(fallback,edge_end_):fallback;}
private:
    static int request(void *,const LyraMessageV3 *,LyraReplyV3 *);
    mutable std::mutex mutex_;
    AudioTransitionSettings settings_;
    const LyraStemAnalysisV3 *stem_=nullptr;
    std::condition_variable changed_;std::deque<AnalysisJob> jobs_;uint64_t analysis_revision_=0;

    std::string cache_directory_;
    int64_t reply_enabled_=0;
    double reply_ms_=0;
    uint64_t edge_generation_=0,reply_generation_=0;int64_t edge_end_=0,reply_end_=0;
    LyraAudioTransitionV3 api_{};
};
int configure_transition(const LyraAudioTransitionV3 *,const AudioTransitionSettings &);
double automatic_transition_ms(const LyraAudioTransitionV3 *);

int64_t transition_audible_end(const LyraAudioTransitionV3 *,uint64_t,int64_t);

int configure_transition_cache(const LyraAudioTransitionV3 *,const std::string &);

int bind_transition_stems(const LyraAudioTransitionV3 *,const LyraStemAnalysisV3 *);
