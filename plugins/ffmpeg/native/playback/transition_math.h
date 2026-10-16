#pragma once
#include "playback/transition_settings.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
struct TransitionBeat { double bpm=0,confidence=0,first_s=0,last_s=0,span_s=0; };
// Small offline-window estimator. Lack of repeatable onsets is explicitly not
// evidence of a beat. No allocation occurs in the sample mixer below.
inline TransitionBeat transition_beat(const int16_t *pcm,size_t frames,int rate,const AudioTransitionSettings &settings){
 TransitionBeat result;if(!pcm || rate<=0)return result;
 const int hop=std::max(1,int(rate/settings.feature_hz));size_t n=frames/hop;result.span_s=double(frames)/rate;
 if(n<2)return result;
 std::vector<double> onset(n,0);double previous=0,mean=0;
 for(size_t i=0;i<n;++i){double energy=0;for(int j=0;j<hop;++j){double x=(double(pcm[(i*hop+j)*2])+pcm[(i*hop+j)*2+1])/65536.;energy+=x*x;}
  energy=std::sqrt(energy/hop);onset[i]=std::max(0.,energy-previous);previous=energy;mean+=onset[i];}
 mean/=n;if(mean<settings.onset_floor)return result;
 double best=0;int best_lag=0;
 for(int lag=std::max(1,int(double(rate)/hop*60/settings.bpm_max));lag<=int(double(rate)/hop*60/settings.bpm_min) && lag<int(n)/2;++lag){double dot=0,a=0,b=0;
  for(size_t i=lag;i<n;++i){double x=onset[i],y=onset[i-lag];dot+=x*y;a+=x*x;b+=y*y;}
  if(a<=0 || b<=0)continue;double score=dot/std::sqrt(a*b);if(score>best){best=score;best_lag=lag;}}
 if(best_lag==0)return result;
 std::vector<size_t> peaks;size_t guard=0;
 for(size_t i=1;i+1<n;++i)if(onset[i]>mean*settings.peak_factor && onset[i]>=onset[i-1] && onset[i]>onset[i+1] && i>=guard){peaks.push_back(i);guard=i+size_t(best_lag)/2;}
 if(peaks.size()<size_t(settings.minimum_onsets))return result;
 double error=0;for(size_t i=1;i<peaks.size();++i){double distance=double(peaks[i]-peaks[i-1])/best_lag;error+=std::abs(distance-std::round(distance));}
 error/=peaks.size()-1;
 result.bpm=60.*rate/(best_lag*hop);result.confidence=best*std::max(0.,1-error*settings.interval_penalty)*std::min(1.,double(peaks.size())/settings.confident_onsets);
 result.first_s=double(peaks.front()*hop)/rate;result.last_s=double(peaks.back()*hop)/rate;return result;
}
class TransitionBands {
 float a_[2]{},b_[2]{},pole_=0,gain_=1,release_=0,limit_=1,begin_=0,end_=1;
public:
 float gain()const{return gain_;}
 void configure(int rate,const AudioTransitionSettings &s){pole_=float(1-std::exp(-6.283185307179586*s.bass_hz/rate));release_=float(1-std::exp(-1000/(rate*s.limiter_release_ms)));limit_=float(s.peak_limit);begin_=float(s.bass_start);end_=float(s.bass_end);a_[0]=a_[1]=b_[0]=b_[1]=0;gain_=1;}
 void step(const float a[2],const float b[2],float progress,float out[2]){
  float p=std::clamp(progress,0.f,1.f),s=p*p*(3-2*p);
  // Separate frequency handover: bass changes near the centre while upper
  // bands retain both phrases. This is not a full-band crossfade.
  float low=std::clamp(((p-begin_)/(end_-begin_)),0.f,1.f);low=low*low*(3-2*low);
  float peak=limit_;
  for(int c=0;c<2;++c){a_[c]+=pole_*(a[c]-a_[c]);b_[c]+=pole_*(b[c]-b_[c]);out[c]=(a[c]-a_[c])*(1-s)+(b[c]-b_[c])*s+a_[c]*(1-low)+b_[c]*low;peak=std::max(peak,std::abs(out[c]));}
  // Sample peak protection, not a claim of intersample true-peak limiting.
  float wanted=limit_/peak;gain_=std::min(wanted,gain_+release_*(wanted-gain_));
  for(int c=0;c<2;++c)out[c]*=gain_;
 }
};
