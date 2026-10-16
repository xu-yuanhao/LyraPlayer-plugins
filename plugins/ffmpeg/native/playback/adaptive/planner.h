#pragma once
#include "playback/adaptive/analysis.h"
namespace lyra::adaptive {
enum class Mode {BeatBlend,EchoBridge,TextureBridge};
struct TransitionPlan {
 Mode mode=Mode::TextureBridge;double duration_ms=0,rate=1,period_s=0,wait_s=0,gain_b=1,echo_ms=0,echo_wet=0,mid_conflict=0;
};
inline TransitionPlan plan(const TrackMap &a,const TrackMap &b,double budget_ms,const AudioTransitionSettings &s){
 TransitionPlan p;p.duration_ms=std::max(budget_ms,s.adaptive.loop_cross_ms);
 bool beat_a=a.confidence>=s.adaptive.confidence && a.bpm>0,beat_b=b.confidence>=s.adaptive.confidence && b.bpm>0;
 double ratio=beat_a && beat_b?a.bpm/b.bpm:1;
 p.gain_b=b.rms>s.onset_floor && a.rms>s.onset_floor?std::clamp(a.rms/b.rms,s.adaptive.gain_min,s.adaptive.gain_max):1;
 p.mid_conflict=std::sqrt(a.mid*b.mid);
 p.period_s=beat_a?60/a.bpm:s.adaptive.loop_ms/1000;
 p.echo_ms=std::min(s.adaptive.echo_max_ms,p.period_s*s.adaptive.echo_beats*1000);
 p.echo_wet=s.adaptive.echo_wet;
 if(beat_a && beat_b && std::abs(ratio-1)<=s.max_rate_delta){
  double beats=std::floor(budget_ms/1000/p.period_s/s.beat_group)*s.beat_group;
  if(beats>=s.beat_group){p.mode=Mode::BeatBlend;p.rate=ratio;p.duration_ms=beats*p.period_s*1000;p.echo_wet=0;return p;}
 }
 p.mode=beat_a?Mode::EchoBridge:Mode::TextureBridge;
 // Echo bridges stay on the outgoing pulse; texture bridges avoid asserting a
 // tempo that the analyzer could not establish.
 if(beat_a){double beats=std::floor(budget_ms/(p.period_s*1000));if(beats>=1)p.duration_ms=beats*p.period_s*1000;}
 return p;
}
inline double alignment(const TrackMap &a,const TrackMap &b,double elapsed_s,const TransitionPlan &p){
 if(p.mode!=Mode::BeatBlend || p.period_s<=0)return 0;
 double until=std::fmod(p.period_s-std::fmod(a.span_s+elapsed_s-a.last_s,p.period_s)-b.first_s/p.rate,p.period_s);
 return until<0?until+p.period_s:until;
}
}
