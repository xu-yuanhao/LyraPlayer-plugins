#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <chrono>

// One command publication; all ramp state belongs to the PCM writer.
class OutputGain {
    struct Parameters { float gain, ms; };
    std::atomic<uint64_t> command_{0};
    uint64_t seen_=0;
    float current_=1, target_=1, step_=0;
    int remaining_=0;
public:
    static constexpr int channels=2;
    void set(float gain,float ms) {
        Parameters p{gain,ms};uint64_t bits;static_assert(sizeof(p)==sizeof(bits));
        std::memcpy(&bits,&p,sizeof(bits));command_.store(bits,std::memory_order_release);
    }
    bool begin(int rate) {
        auto bits=command_.load(std::memory_order_acquire);
        if(bits!=seen_){
            seen_=bits;Parameters p;std::memcpy(&p,&bits,sizeof(p));target_=p.gain;
            remaining_=int(float(rate)*std::chrono::duration<float>(std::chrono::duration<float,std::milli>(p.ms)).count());
            if(remaining_>0)step_=(target_-current_)/remaining_;
            else current_=target_;
        }
        return current_!=1.f || remaining_>0;
    }
    void apply(const int16_t *src,int16_t *dst,int frames) const {
        for(int i=0;i<frames;++i){
            float gain=current_+step_*std::min(i+1,remaining_);
            for(int channel=0;channel<channels;++channel)dst[i*channels+channel]=int16_t(src[i*channels+channel]*gain);
        }
    }
    void consume(int frames) {
        int used=std::min(frames,remaining_);current_+=step_*used;remaining_-=used;
        if(!remaining_)current_=target_;
    }
};
