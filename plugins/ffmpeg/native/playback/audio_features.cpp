#include "playback/audio_features.h"
#include <algorithm>
#include <cmath>
static_assert(std::atomic<uint64_t>::is_always_lock_free, "audio summaries must be lock-free");

void AudioFeatureHistory::LowPass::setup(double hz, double rate) {
    double w = 6.283185307179586 * hz / rate, c = std::cos(w), a = std::sin(w) / 1.4142135623730951;
    b0 = (1 - c) * .5 / (1 + a);
    b1 = (1 - c) / (1 + a);
    b2 = b0;
    a1 = -2 * c / (1 + a);
    a2 = (1 - a) / (1 + a);
    z1 = z2 = 0;
}
double AudioFeatureHistory::LowPass::step(double x) {
    double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
}
void AudioFeatureHistory::LowPass::clear_tiny() {
    if (std::abs(z1) < 1e-24)
        z1 = 0;
    if (std::abs(z2) < 1e-24)
        z2 = 0;
}
void AudioFeatureHistory::reset(int rate, const AudioFeatureSettings &settings) {
    writer_epoch_ = epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    sample_rate_.store(rate, std::memory_order_release);
    window_ = std::max(1, (int)std::lround(rate * settings.hop_ms / 1000));
    count_ = 0;
    std::fill_n(sum_, 4, 0);
    std::fill_n(smooth_, 4, 0);
    reference_ = .08f;
    slow_bass_ = slow_energy_ = accent_ = 0;
    accent_gain_ = settings.accent_gain;
    for (int c = 0; c < 2; ++c) {
        low_[c].setup(std::clamp((double)settings.bass_hz, 20., rate * .2), rate);
        wide_[c].setup(std::clamp((double)settings.wide_hz, 200., rate * .35), rate);
    }
}
void AudioFeatureHistory::feed(const int16_t *stereo, int frames, int64_t first_frame, int64_t output_frame) {
    if (output_frame < 0)
        output_frame = first_frame;
    if (!stereo || window_ < 1)
        return;
    for (int i = 0; i < frames; ++i) {
        // Sum channel powers independently: opposite-phase stereo must remain audible to the analyzer.
        for (int c = 0; c < 2; ++c) {
            double x = stereo[i * 2 + c] / 32768., l = low_[c].step(x), w = wide_[c].step(x), m = w - l, h = x - w;
            sum_[0] += x * x;
            sum_[1] += l * l;
            sum_[2] += m * m;
            sum_[3] += h * h;
        }
        if (++count_ == window_)
            publish(first_frame + i + 1, output_frame + i + 1);
    }
}
void AudioFeatureHistory::publish(int64_t frame, int64_t output_frame) {
    float rms[4];
    for (int i = 0; i < 4; ++i)
        rms[i] = std::sqrt(sum_[i] / (count_ * 2));
    float gate = std::clamp((rms[0] - .0008f) / .006f, 0.f, 1.f);
    reference_ = std::max(.02f, std::max(rms[0], reference_ * .998f));
    // Match rewrite novelty weighting; distinguish sustained bass from a new attack.
    float novelty = std::max(0.f, rms[1] - slow_bass_ * 1.12f) + .45f * std::max(0.f, rms[0] - slow_energy_ * 1.12f);
    float hit = std::clamp(novelty * accent_gain_ / (reference_ * .45f + .006f), 0.f, 1.f) * gate;
    slow_bass_ += (rms[1] - slow_bass_) * .10f;
    slow_energy_ += (rms[0] - slow_energy_) * .10f;
    if (slow_bass_ < 1e-12f)
        slow_bass_ = 0;
    if (slow_energy_ < 1e-12f)
        slow_energy_ = 0;
    accent_ = std::max(hit, accent_ * .88f);
    if (accent_ < 1e-7f)
        accent_ = 0;
    float values[5];
    for (int i = 0; i < 4; ++i) {
        float target = std::clamp(rms[i] / reference_, 0.f, 1.f) * gate;
        smooth_[i] += (target - smooth_[i]) * (target > smooth_[i] ? .55f : .065f);
        if (smooth_[i] < 1e-7f)
            smooth_[i] = 0;
        values[i] = smooth_[i];
    }
    values[4] = accent_;
    uint64_t packed = 0;
    for (int i = 0; i < 5; ++i)
        packed |= (uint64_t)std::lround(std::clamp(values[i], 0.f, 1.f) * 4095) << (12 * i);
    uint64_t index = published_.load(std::memory_order_relaxed);
    Slot &slot = slots_[index % capacity];
    slot.sequence.store(index * 2 + 1, std::memory_order_seq_cst);
    slot.epoch.store(writer_epoch_, std::memory_order_relaxed);
    slot.frame.store(frame, std::memory_order_relaxed);
    slot.output_frame.store(output_frame, std::memory_order_relaxed);
    slot.values.store(packed, std::memory_order_relaxed);
    slot.sequence.store(index * 2 + 2, std::memory_order_seq_cst);
    published_.store(index + 1, std::memory_order_release);
    count_ = 0;
    std::fill_n(sum_, 4, 0);
    for (int c = 0; c < 2; ++c) {
        low_[c].clear_tiny();
        wide_[c].clear_tiny();
    }
}
bool AudioFeatureHistory::read(uint64_t index, uint64_t *epoch, int64_t *frame, AudioFeatures *out,
                               bool output_clock) const {
    const Slot &slot = slots_[index % capacity];
    uint64_t stamp = slot.sequence.load(std::memory_order_seq_cst);
    if (stamp != index * 2 + 2)
        return false;
    *epoch = slot.epoch.load(std::memory_order_relaxed);
    *frame = (output_clock ? slot.output_frame : slot.frame).load(std::memory_order_relaxed);
    uint64_t packed = slot.values.load(std::memory_order_relaxed);
    float v[5];
    for (int i = 0; i < 5; ++i)
        v[i] = ((packed >> (12 * i)) & 4095) / 4095.f;
    *out = {v[0], v[1], v[2], v[3], v[4]};
    return slot.sequence.load(std::memory_order_seq_cst) == stamp;
}
AudioFeatures AudioFeatureHistory::sample(int64_t frame, bool *ready) const {
    return sample_clock(frame, ready, false);
}
AudioFeatures AudioFeatureHistory::sample_output(int64_t frame, bool *ready) const {
    return sample_clock(frame, ready, true);
}
AudioFeatures AudioFeatureHistory::sample_clock(int64_t frame, bool *ready, bool output_clock) const {
    if (ready)
        *ready = false;
    uint64_t gen = epoch_.load(std::memory_order_acquire), end = published_.load(std::memory_order_acquire);
    uint64_t lo = end > capacity ? end - capacity : 0, hi = end;
    uint64_t epoch = 0;
    int64_t at = 0;
    AudioFeatures out;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (!read(mid, &epoch, &at, &out, output_clock))
            return {};
        if (epoch < gen || (epoch == gen && at <= frame))
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0 || !read(lo - 1, &epoch, &at, &out, output_clock) || epoch != gen || at > frame ||
        frame - at > sample_rate_.load(std::memory_order_acquire) / 5 || gen != epoch_.load(std::memory_order_acquire))
        return {};
    if (ready)
        *ready = true;
    return out;
}
