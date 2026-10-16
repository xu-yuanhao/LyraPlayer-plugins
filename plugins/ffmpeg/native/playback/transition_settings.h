#pragma once
#include <cmath>
struct AudioEdgeSettings {
    bool enabled=false;
    double rms_db=0,peak_db=0,window_ms=0,minimum_ms=0,guard_ms=0,scan_ms=0;
};
inline bool audio_edges_valid(const AudioEdgeSettings &s){
    const double values[]={s.rms_db,s.peak_db,s.window_ms,s.minimum_ms,s.guard_ms,s.scan_ms};
    for(double v:values)if(!std::isfinite(v))return false;
    bool empty=true;for(double v:values)empty=empty && v==0;
    return (!s.enabled && empty) || (s.rms_db>=-120 && s.rms_db<=-40 && s.peak_db>=s.rms_db && s.peak_db<=-30 &&
        s.window_ms>=1 && s.window_ms<=50 && s.minimum_ms>=s.window_ms && s.minimum_ms<=2000 &&
        s.guard_ms>=s.window_ms && s.guard_ms<s.scan_ms && s.scan_ms>=s.minimum_ms && s.scan_ms<=10000);
}
struct AdaptiveTransitionSettings {
 bool enabled=false;
 double confidence=0,split_hz=0,smooth_ms=0,tolerance_ms=0,phase_wait_ms=0;
 double echo_wet=0,echo_feedback=0,echo_beats=0,echo_max_ms=0,echo_low_hz=0;
 double filter_min_hz=0,filter_max_hz=0,gain_min=0,gain_max=0,mid_start=0,mid_end=0;
 double loop_ms=0,bridge_max_ms=0,bridge_fade_ms=0,loop_cross_ms=0,cache_entries=0;
};
inline bool adaptive_transition_valid(const AdaptiveTransitionSettings &s){
 const double v[]={s.confidence,s.split_hz,s.smooth_ms,s.tolerance_ms,s.phase_wait_ms,s.echo_wet,s.echo_feedback,s.echo_beats,s.echo_max_ms,s.echo_low_hz,s.filter_min_hz,s.filter_max_hz,s.gain_min,s.gain_max,s.mid_start,s.mid_end,s.loop_ms,s.bridge_max_ms,s.bridge_fade_ms,s.loop_cross_ms,s.cache_entries};
 bool empty=true;for(double x:v){if(!std::isfinite(x))return false;empty=empty && x==0;}
 return (!s.enabled && empty) || (s.confidence>0 && s.confidence<=1 && s.split_hz>=500 && s.split_hz<=8000 && s.smooth_ms>=1 && s.smooth_ms<=100 && s.tolerance_ms>=1 && s.tolerance_ms<=100 && s.phase_wait_ms>=1 && s.phase_wait_ms<=1000 && s.echo_wet>=0 && s.echo_wet<=.65 && s.echo_feedback>=0 && s.echo_feedback<=.75 && s.echo_beats>0 && s.echo_beats<=2 && s.echo_max_ms>=50 && s.echo_max_ms<=2000 && s.echo_low_hz>=50 && s.echo_low_hz<=4000 && s.filter_min_hz>=100 && s.filter_min_hz<s.filter_max_hz && s.filter_max_hz<=20000 && s.gain_min>=.5 && s.gain_min<=1 && s.gain_max>=1 && s.gain_max<=2 && s.mid_start>0 && s.mid_start<s.mid_end && s.mid_end<1 && s.loop_ms>=100 && s.loop_ms<=1000 && s.bridge_max_ms>=s.loop_ms && s.bridge_max_ms<=2000 && s.bridge_fade_ms>=20 && s.bridge_fade_ms<=s.bridge_max_ms && s.loop_cross_ms>=1 && s.loop_cross_ms<=s.loop_ms/2 && s.cache_entries>=1 && s.cache_entries<=4096 && s.cache_entries==std::floor(s.cache_entries));
}
struct FullTrackSettings {
 bool enabled=false,library=false;
 double hop_ms=0;
 double novelty_ms=0;
 double novelty_threshold=0;
 double phrase_min_ms=0;
 double max_ms=0;
 double cache_mb=0;
 double queue_limit=0;
 double idle_ms=0;
 double vocal_threshold=0;
 double vocal_floor_db=0;
 double vocal_smooth_ms=0;
 double vocal_guard_ms=0;
 double vocal_fade_ms=0;
 double wait_ms=0;
 double intro_ms=0;
 double stem_window_ms=0;
 double confidence=0,chroma_low_hz=0,chroma_high_hz=0,worker_nice=0;
};
inline bool full_track_valid(const FullTrackSettings &s){
 const double values[]={s.hop_ms,s.novelty_ms,s.novelty_threshold,s.phrase_min_ms,s.max_ms,s.cache_mb,s.queue_limit,s.idle_ms,s.vocal_threshold,s.vocal_floor_db,s.vocal_smooth_ms,s.vocal_guard_ms,s.vocal_fade_ms,s.wait_ms,s.intro_ms,s.stem_window_ms,s.confidence,s.chroma_low_hz,s.chroma_high_hz,s.worker_nice};
 bool empty=true;for(double v:values){if(!std::isfinite(v))return false;empty=empty && v==0;}
 return (!s.enabled && !s.library && empty) || (s.hop_ms>=100 && s.hop_ms<=500 && std::fmod(s.hop_ms,10)==0 && s.novelty_ms>=s.hop_ms*4 && s.novelty_ms<=30000 && s.novelty_threshold>0 && s.novelty_threshold<=1 && s.phrase_min_ms>=1000 && s.phrase_min_ms<=30000 && s.max_ms>=10000 && s.max_ms<=7200000 && s.cache_mb>=32 && s.cache_mb<=4096 && s.queue_limit>=1 && s.queue_limit<=16384 && s.queue_limit==std::floor(s.queue_limit) && s.idle_ms>=1 && s.idle_ms<=100 && s.vocal_threshold>0 && s.vocal_threshold<1 && s.vocal_floor_db>=-100 && s.vocal_floor_db<=-20 && s.vocal_smooth_ms>=s.hop_ms && s.vocal_smooth_ms<=2000 && s.vocal_guard_ms>=0 && s.vocal_guard_ms<=500 && s.vocal_fade_ms>=50 && s.vocal_fade_ms<=1000 && s.wait_ms>=0 && s.wait_ms<=2000 && s.intro_ms>=0 && s.intro_ms<=8000 && s.stem_window_ms>=8000 && s.stem_window_ms<=16000 && s.confidence>0 && s.confidence<=1 && s.chroma_low_hz>=20 && s.chroma_low_hz<=1000 && s.chroma_high_hz>s.chroma_low_hz && s.chroma_high_hz<=20000 && s.worker_nice>=0 && s.worker_nice<=19 && s.worker_nice==std::floor(s.worker_nice));
}
struct AudioTransitionSettings {
    FullTrackSettings full;
    AdaptiveTransitionSettings adaptive;
    AudioEdgeSettings edges;
    bool enabled=false;
    double automatic_ms=0;
    double manual_ms=0;
    double bridge_ms=0;
    double analysis_ms=0;
    double confidence=0;
    double max_rate_delta=0;
    double bass_hz=0;
    double return_ms=0;
    double feature_hz=0;
    double bpm_min=0;
    double bpm_max=0;
    double onset_floor=0;
    double peak_factor=0;
    double minimum_onsets=0;
    double confident_onsets=0;
    double interval_penalty=0;
    double bass_start=0;
    double bass_end=0;
    double limiter_release_ms=0;
    double peak_limit=0;
    double maximum_track_fraction=0;
    double beat_group=0;
};
inline bool audio_transition_valid(const AudioTransitionSettings &s){
    const double values[]={s.automatic_ms,s.manual_ms,s.bridge_ms,s.analysis_ms,s.confidence,s.max_rate_delta,s.bass_hz,s.return_ms,s.feature_hz,s.bpm_min,s.bpm_max,s.onset_floor,s.peak_factor,s.minimum_onsets,s.confident_onsets,s.interval_penalty,s.bass_start,s.bass_end,s.limiter_release_ms,s.peak_limit,s.maximum_track_fraction,s.beat_group};
    for(double value:values)if(!std::isfinite(value)||value<=0)return false;
    return full_track_valid(s.full) && adaptive_transition_valid(s.adaptive) && audio_edges_valid(s.edges) && s.automatic_ms<=8000 && s.manual_ms<=8000 && s.bridge_ms<=2000 && s.analysis_ms<=8000 &&
        s.return_ms<=16000 && s.confidence<=1 && s.max_rate_delta<=.1 && s.bass_hz<=1000 &&
        s.feature_hz>=20 && s.feature_hz<=1000 && s.bpm_min>=30 && s.bpm_max>s.bpm_min && s.bpm_max<=300 &&
        s.peak_factor<=20 && s.minimum_onsets>=3 && s.minimum_onsets<=s.confident_onsets && s.confident_onsets<=32 &&
        s.minimum_onsets==std::floor(s.minimum_onsets) && s.confident_onsets==std::floor(s.confident_onsets) &&
        s.interval_penalty<=32 && s.bass_start<s.bass_end && s.bass_end<1 && s.limiter_release_ms<=2000 && s.peak_limit<=1 && s.maximum_track_fraction<1 && float(s.peak_limit)>0 && float(s.bass_start)<float(s.bass_end) &&
        s.analysis_ms*s.feature_hz/1000>=s.minimum_onsets && s.beat_group>=1 && s.beat_group<=16 && s.beat_group==std::floor(s.beat_group);
}
