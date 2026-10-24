#pragma once
#include "playback/transition_settings.h"
#include "plugin/transition_contracts.h"
#include <mutex>
class AudioTransitionControl {
public:
    AudioTransitionControl();
    const LyraAudioTransitionV3 *interface()const{return &api_;}
    AudioTransitionSettings settings()const{std::lock_guard<std::mutex> lock(mutex_);return settings_;}
private:
    static int request(void *,const LyraMessageV3 *,LyraReplyV3 *);
    mutable std::mutex mutex_;
    AudioTransitionSettings settings_;
    int64_t reply_enabled_=0;
    double reply_ms_=0;
    LyraAudioTransitionV3 api_{};
};
int configure_transition(const LyraAudioTransitionV3 *,const AudioTransitionSettings &);
double automatic_transition_ms(const LyraAudioTransitionV3 *);
