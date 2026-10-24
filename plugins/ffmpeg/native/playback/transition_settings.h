#pragma once
#include <cmath>
// A track change blends the outgoing decode into the incoming one with a plain
// linear fade: short automatic overlap near the end of a track, a longer one on
// manual skips. No analysis, no tempo work, no caches.
struct AudioTransitionSettings {
    bool enabled=false;
    double automatic_ms=0;
    double manual_ms=0;
};
inline bool audio_transition_valid(const AudioTransitionSettings &s){
    return std::isfinite(s.automatic_ms) && std::isfinite(s.manual_ms) &&
        s.automatic_ms>0 && s.manual_ms>0 && s.automatic_ms<=8000 && s.manual_ms<=8000;
}
// Linear fade of the outgoing decode into the incoming one. Linear keeps the
// sum at full scale for correlated material, so no limiter is involved.
inline void audio_crossfade(int16_t *incoming,const int16_t *outgoing,int frames,
                            int64_t blended,int64_t total){
    if(total<1)total=1;
    for(int i=0;i<frames;++i){
        double weight=double(blended+i)/double(total);
        if(weight>1)weight=1;
        if(weight<0)weight=0;
        for(int c=0;c<2;++c){
            double value=double(incoming[i*2+c])*weight;
            if(outgoing)value+=double(outgoing[i*2+c])*(1.-weight);
            long rounded=std::lround(value);
            if(rounded<-32768)rounded=-32768;
            if(rounded>32767)rounded=32767;
            incoming[i*2+c]=int16_t(rounded);
        }
    }
}
