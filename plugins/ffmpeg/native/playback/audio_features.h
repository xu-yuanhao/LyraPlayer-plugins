#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

struct AudioFeatures {
    float energy = 0, bass = 0, mid = 0, high = 0, accent = 0;
};
struct AudioFeatureSettings {
    float bass_hz = 0, wide_hz = 0, hop_ms = 0, accent_gain = 0;
};
// Single audio-writer producer; atomic, bounded history sampled at the presentation clock.
class AudioFeatureHistory {
  public:
    void reset(int rate, const AudioFeatureSettings &settings);
    void feed(const int16_t *stereo, int frames, int64_t first_frame, int64_t output_frame = -1);
    AudioFeatures sample(int64_t frame, bool *ready = nullptr) const;
    AudioFeatures sample_output(int64_t frame, bool *ready = nullptr) const;
    uint64_t epoch() const { return epoch_.load(std::memory_order_acquire); }

  private:
    struct LowPass {
        double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void setup(double hz, double rate);
        double step(double x);
        void clear_tiny();
    };
    struct Slot {
        std::atomic<uint64_t> sequence{0}, epoch{0}, values{0};
        std::atomic<int64_t> frame{0}, output_frame{0};
    };
    static constexpr std::size_t capacity = 512;
    std::array<Slot, capacity> slots_{};
    std::atomic<uint64_t> epoch_{0}, published_{0};
    std::atomic<int> sample_rate_{0};
    LowPass low_[2], wide_[2];
    double sum_[4]{};
    float smooth_[4]{}, reference_ = .08f, slow_bass_ = 0, slow_energy_ = 0, accent_ = 0, accent_gain_ = 0;
    int count_ = 0, window_ = 0;
    uint64_t writer_epoch_ = 0;
    bool read(uint64_t index, uint64_t *epoch, int64_t *frame, AudioFeatures *out, bool output_clock = false) const;
    void publish(int64_t frame, int64_t output_frame);
    AudioFeatures sample_clock(int64_t frame, bool *ready, bool output_clock) const;
};
