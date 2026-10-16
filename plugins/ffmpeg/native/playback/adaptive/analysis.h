#pragma once
#include "playback/transition_math.h"
#include <array>
#include <limits>
namespace lyra::adaptive {
// A bounded, versioned region map. Unknown key/vocal/phrase information is not
// fabricated from spectrum. A full-track index can contribute additional maps.
struct TrackMap {
 uint32_t version=1,beats=0;
 double bpm=0,confidence=0,first_s=0,last_s=0,span_s=0;
 double rms=0,bass=0,mid=0,air=0,attack=0;
 std::array<double,64> grid{};
 TransitionBeat rhythm()const{return {bpm,confidence,first_s,last_s,span_s};}
};
inline TrackMap analyze(const int16_t *pcm,size_t frames,int rate,const AudioTransitionSettings &s){
 TrackMap map;map.span_s=double(frames)/rate;if(!frames)return map;
 int hop=std::max(1,int(rate/s.feature_hz));size_t count=frames/hop;if(count<2)return map;
 std::vector<double> low(count),high(count),broad(count),onset(count);
 double p=1-std::exp(-6.283185307179586*s.bass_hz/rate),q=1-std::exp(-6.283185307179586*s.adaptive.split_hz/rate);
 double a[2]{},b[2]{},total=0,lo=0,mi=0,hi=0;
 for(size_t i=0;i<count;++i){double e=0,l=0,h=0;
  for(int j=0;j<hop;++j)for(int c=0;c<2;++c){double x=pcm[(i*hop+j)*2+c]/32768.;a[c]+=p*(x-a[c]);b[c]+=q*(x-b[c]);double m=b[c]-a[c],t=x-b[c];l+=a[c]*a[c];h+=t*t;e+=x*x;lo+=a[c]*a[c];mi+=m*m;hi+=t*t;}
  low[i]=std::sqrt(l/(hop*2));high[i]=std::sqrt(h/(hop*2));broad[i]=std::sqrt(e/(hop*2));total+=e;
 }
 map.rms=std::sqrt(total/(count*hop*2));double sum=lo+mi+hi+s.onset_floor;map.bass=lo/sum;map.mid=mi/sum;map.air=hi/sum;
 int smooth=std::max(1,int(s.adaptive.smooth_ms*s.feature_hz/1000));
 // Smooth energy before differentiating. Rectifying carrier ripple first
 // mistakes sustained singing and bass oscillation for a cloud of attacks.
 double rolling_low=0,rolling_high=0,rolling_broad=0,previous_low=0,previous_high=0,previous_broad=0;
 double average_low=0,average_high=0,average_broad=0;
 for(size_t i=0;i<count;++i){average_low+=low[i];average_high+=high[i];average_broad+=broad[i];}
 average_low=average_low/count+s.onset_floor;average_high=average_high/count+s.onset_floor;average_broad=average_broad/count+s.onset_floor;
 double mean=0;
 for(size_t i=0;i<count;++i){
  rolling_low+=low[i];rolling_high+=high[i];rolling_broad+=broad[i];
  if(i>=size_t(smooth)){rolling_low-=low[i-smooth];rolling_high-=high[i-smooth];rolling_broad-=broad[i-smooth];}
  double l=rolling_low/smooth,h=rolling_high/smooth,e=rolling_broad/smooth;
  onset[i]=std::max(0.,l-previous_low)/average_low+std::max(0.,h-previous_high)/average_high+std::max(0.,e-previous_broad)/average_broad;
  previous_low=l;previous_high=h;previous_broad=e;mean+=onset[i];
 }
 mean/=count;map.attack=mean;if(mean<s.onset_floor || map.rms<s.onset_floor)return map;
 std::vector<double> flux(count);for(size_t i=0;i<count;++i)flux[i]=std::max(0.,onset[i]-mean);
 int minimum=std::max(1,int(s.feature_hz*60/s.bpm_max)),maximum=std::min(int(count/2),int(s.feature_hz*60/s.bpm_min));
 double best=0;int lag_best=0;
 for(int lag=minimum;lag<=maximum;++lag){double dot=0,left=0,right=0;
  for(size_t i=lag;i<count;++i){dot+=flux[i]*flux[i-lag];left+=flux[i]*flux[i];right+=flux[i-lag]*flux[i-lag];}
  double score=left>0 && right>0?dot/std::sqrt(left*right):0;
  if(score>best+std::numeric_limits<double>::epsilon()*count){best=score;lag_best=lag;}
 }
 if(!lag_best)return map;
 // Search a pulse grid, then measure how many expected beats have evidence.
 int tolerance=std::max(1,int(s.adaptive.tolerance_ms*s.feature_hz/1000));double phase_best=-1;int phase=0;
 for(int start=0;start<lag_best;++start){double score=0;
  for(int i=start;i<int(count);i+=lag_best){double peak=0;for(int j=std::max(0,i-tolerance);j<=std::min(int(count)-1,i+tolerance);++j)peak=std::max(peak,flux[j]);score+=peak;}
  if(score>phase_best){phase_best=score;phase=start;}
 }
 double maximum_flux=*std::max_element(flux.begin(),flux.end());int supported=0,expected=0;
 for(int i=phase;i<int(count);i+=lag_best){++expected;double peak=0;int at=i;
  for(int j=std::max(0,i-tolerance);j<=std::min(int(count)-1,i+tolerance);++j)if(flux[j]>peak){peak=flux[j];at=j;}
  if(peak>mean && peak>maximum_flux/s.peak_factor){++supported;if(map.beats<map.grid.size())map.grid[map.beats++]=double(at*hop)/rate;}
 }
 if(supported<int(s.minimum_onsets) || !map.beats)return map;
 map.bpm=60.*rate/(lag_best*hop);map.confidence=best*double(supported)/expected;
 map.first_s=map.grid[0];map.last_s=map.grid[map.beats-1];return map;
}
}
