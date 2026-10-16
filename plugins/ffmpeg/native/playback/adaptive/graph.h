#pragma once
#include "playback/adaptive/planner.h"
namespace lyra::adaptive {
inline float curve(float x){x=std::clamp(x,0.f,1.f);return x*x*(3-2*x);}
class TransitionGraph {
 std::vector<float> echo_;size_t at_=0,delay_=1;float low_a_[2]{},low_b_[2]{},filter_a_[2]{},send_low_[2]{},gain_=1;
 float bass_pole_=0,filter_pole_=0,send_pole_=0,release_=0,limit_=1;TransitionBands bands_;
 AudioTransitionSettings settings_;TransitionPlan plan_;
public:
 void prepare(int rate,const AudioTransitionSettings &s){settings_=s;echo_.assign(size_t(std::ceil(rate*s.adaptive.echo_max_ms/1000))*2,0);}
 void arm(int rate,const TransitionPlan &plan,const AudioTransitionSettings &s,const std::vector<int16_t> &history,size_t history_at,size_t history_count){
  plan_=plan;settings_=s;at_=0;delay_=std::min(echo_.size()/2,size_t(std::max(1.,rate*plan.echo_ms/1000)));
  std::fill(echo_.begin(),echo_.end(),0);std::fill_n(low_a_,2,0);std::fill_n(low_b_,2,0);std::fill_n(filter_a_,2,0);std::fill_n(send_low_,2,0);gain_=1;
  bass_pole_=float(1-std::exp(-6.283185307179586*s.bass_hz/rate));send_pole_=float(1-std::exp(-6.283185307179586*s.adaptive.echo_low_hz/rate));release_=float(1-std::exp(-1000/(rate*s.limiter_release_ms)));limit_=float(s.peak_limit);bands_.configure(rate,s);
  size_t samples=std::min(history_count,delay_*2);if(!history.empty())for(size_t i=0;i<samples;++i){size_t from=(history_at+history.size()-samples+i)%history.size();int c=int(i%2);float x=history[from]/32768.f;send_low_[c]+=send_pole_*(x-send_low_[c]);low_a_[c]+=bass_pole_*(x-low_a_[c]);filter_a_[c]=x;echo_[delay_*2-samples+i]=x-send_low_[c];}
 }
 void block(float progress,int rate){if(progress>=1 || plan_.mode==Mode::BeatBlend)return;double cutoff=settings_.adaptive.filter_max_hz*std::pow(settings_.adaptive.filter_min_hz/settings_.adaptive.filter_max_hz,curve(progress));filter_pole_=float(1-std::exp(-6.283185307179586*cutoff/rate));}
 void step(const float a[2],const float b[2],float progress,float out[2]){
  float p=std::clamp(progress,0.f,1.f);
  if(p>=1){float peak=std::max({limit_,std::abs(b[0]),std::abs(b[1])});float wanted=limit_/peak;gain_=std::min(wanted,gain_+release_*(wanted-gain_));for(int c=0;c<2;++c)out[c]=b[c]*gain_;return;}
  float level=float(1+(plan_.gain_b-1)*(1-curve(p)));float incoming[2]={b[0]*level,b[1]*level};
  if(plan_.mode==Mode::BeatBlend){bands_.step(a,incoming,p,out);gain_=bands_.gain();return;}
  float old_gain=1-curve(p/float(settings_.adaptive.mid_end));
  // Introduce the new texture immediately at an equal-power level. Spectral
  // mid energy is not a vocal detector and must not mute the incoming song.
  float new_gain=std::sqrt(curve(p/float(settings_.adaptive.mid_end)));
  // Exchange bass from the start as well: piano/bass intros must be audible
  // before the middle of the blend, rather than waiting for a late bass gate.
  float bass=curve(p/float(settings_.bass_end));
  float wet=float(plan_.echo_wet)*16*p*p*(1-p)*(1-p);
  float delayed[2]={echo_[at_*2],echo_[at_*2+1]};float peak=limit_;
  for(int c=0;c<2;++c){
   low_a_[c]+=bass_pole_*(a[c]-low_a_[c]);low_b_[c]+=bass_pole_*(incoming[c]-low_b_[c]);
   float old=a[c]-low_a_[c]*bass;filter_a_[c]+=filter_pole_*(old-filter_a_[c]);
   send_low_[c]+=send_pole_*(a[c]-send_low_[c]);float send=(a[c]-send_low_[c])*old_gain;
   echo_[at_*2+c]=send+delayed[1-c]*float(settings_.adaptive.echo_feedback);
   out[c]=filter_a_[c]*old_gain+(incoming[c]-low_b_[c])*new_gain+low_b_[c]*bass+delayed[c]*wet;
   peak=std::max(peak,std::abs(out[c]));
  }
  at_=(at_+1)%delay_;float wanted=limit_/peak;gain_=std::min(wanted,gain_+release_*(wanted-gain_));for(int c=0;c<2;++c)out[c]*=gain_;
 }
};
}
