#include "playback/audio_features.h"
#define JOIN_IMPL(a,b) a##b
#define JOIN(a,b) JOIN_IMPL(a,b)
#define FN(name) JOIN(FEATURE_PREFIX,name)
extern "C" void *FN(_create)(int rate, AudioFeatureSettings settings) {
    auto *history = new AudioFeatureHistory;
    history->reset(rate, settings);
    return history;
}
extern "C" void FN(_feed)(void *history, const int16_t *samples, int count, int64_t media, int64_t output) {
    static_cast<AudioFeatureHistory *>(history)->feed(samples, count, media, output);
}
extern "C" void FN(_sample)(void *history, int64_t at, bool output, AudioFeatures *value, bool *ready) {
    auto *p = static_cast<AudioFeatureHistory *>(history);
    *value = output ? p->sample_output(at, ready) : p->sample(at, ready);
}
extern "C" void FN(_destroy)(void *history) { delete static_cast<AudioFeatureHistory *>(history); }
