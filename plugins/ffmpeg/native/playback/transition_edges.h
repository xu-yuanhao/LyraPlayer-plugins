#pragma once
#include "playback/transition_settings.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>
#include <chrono>
// Fixed windows inspect both channels independently; opposite-phase stereo
// must never cancel into a false silence decision.
struct TransitionEdgeScan {
    int window=1,used=0;int64_t frames=0,first=-1,last=-1;double energy=0,peak=0,rms_limit=0,peak_limit=0;
    TransitionEdgeScan(int rate,const AudioEdgeSettings &s):window(std::max(1,int(rate*s.window_ms/1000))),rms_limit(std::pow(10.,s.rms_db/20)),peak_limit(std::pow(10.,s.peak_db/20)){}
    void finish(){if(!used)return;if(energy/used>rms_limit*rms_limit || peak>peak_limit){if(first<0)first=frames-used;last=frames;}used=0;energy=peak=0;}
    void feed(const int16_t *pcm,size_t count){for(size_t i=0;i<count;++i){double a=pcm[i*2]/32768.,b=pcm[i*2+1]/32768.;energy+=(a*a+b*b)*.5;peak=std::max({peak,std::abs(a),std::abs(b)});++frames;if(++used==window)finish();}}
};
inline int64_t transition_head_skip(const std::vector<int16_t> &pcm,int rate,const AudioEdgeSettings &s){
    if(!s.enabled)return 0;TransitionEdgeScan scan(rate,s);scan.feed(pcm.data(),pcm.size()/2);scan.finish();
    if(scan.first<0 || scan.first*1000<rate*s.minimum_ms)return 0;
    return std::max<int64_t>(0,scan.first-int64_t(rate*s.guard_ms/1000));
}
// Decoder owns its buffers and fd. Scan the bounded tail with that same decoder,
// then rewind it before priming the head: never create a third decoder.
template<class Decoder> bool prepare_transition_edges(Decoder &source,int rate,const AudioTransitionSettings &s,const std::atomic<bool> &cancel){
    source.audible_end_us=source.duration_us;
    if(s.edges.enabled && source.duration_us>0){
        int64_t start=std::max<int64_t>(0,source.duration_us-int64_t(s.edges.scan_ms*1000));
        bool positioned=start==0 || source.seek(start);
        if(positioned){
            TransitionEdgeScan scan(rate,s.edges);bool end=false;int result=1;
            int64_t budget=int64_t(rate*(s.edges.scan_ms+s.analysis_ms)/1000);
            auto deadline=std::chrono::steady_clock::now()+std::chrono::duration<double,std::milli>(s.analysis_ms);
            while(!cancel.load() && scan.frames<=budget && std::chrono::steady_clock::now()<deadline){
                result=source.read(rate);if(result<0)break;
                scan.feed(source.stereo.data(),source.stereo.size()/2);
                if(result==0){end=true;break;}
            }
            scan.finish();
            if(end && source.first_media_us>=0){
                int64_t last=scan.last<0?source.first_media_us:source.first_media_us+scan.last*1000000/rate;
                // A wholly silent short track retains its original timeline.
                if((scan.last>=0 || start>0) && source.duration_us-last>=s.edges.minimum_ms*1000)
                    source.audible_end_us=std::min(source.duration_us,last+int64_t(s.edges.guard_ms*1000));
            }
        }
        if(cancel.load() || !source.seek(0))return false;
    }
    size_t target=size_t(rate*s.analysis_ms/1000)*2;
    size_t maximum=s.edges.enabled?std::max(target,size_t(rate*s.edges.scan_ms/1000)*2):target;
    source.prepared.reserve(target+65536);
    TransitionEdgeScan head(rate,s.edges);
    auto deadline=std::chrono::steady_clock::now()+std::chrono::duration<double,std::milli>(s.analysis_ms);
    while(!cancel.load() && source.prepared.size()<maximum && std::chrono::steady_clock::now()<deadline){
        int result=source.read(rate);if(result<0)return false;
        if(s.edges.enabled)head.feed(source.stereo.data(),source.stereo.size()/2);
        source.prepared.insert(source.prepared.end(),source.stereo.begin(),source.stereo.end());source.stereo_pos=source.stereo.size();
        if(result==0)break;
        if(source.prepared.size()>=target){
            if(!s.edges.enabled || head.first>=0)break;
        }
    }
    if(cancel.load() || source.prepared.empty())return false;
    auto skip=transition_head_skip(source.prepared,rate,s.edges);
    source.prepared_pos=size_t(skip)*2;source.delivered_frames=skip;
    return true;
}
