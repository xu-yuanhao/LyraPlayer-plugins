#include "playback/transition_control.h"
#include <cmath>
#include <array>
#include <cstring>
#include <unistd.h>
namespace {
const auto members=std::array{
 &AudioTransitionSettings::automatic_ms,
 &AudioTransitionSettings::manual_ms,
 &AudioTransitionSettings::bridge_ms,
 &AudioTransitionSettings::analysis_ms,
 &AudioTransitionSettings::confidence,
 &AudioTransitionSettings::max_rate_delta,
 &AudioTransitionSettings::bass_hz,
 &AudioTransitionSettings::return_ms,
 &AudioTransitionSettings::feature_hz,
 &AudioTransitionSettings::bpm_min,
 &AudioTransitionSettings::bpm_max,
 &AudioTransitionSettings::onset_floor,
 &AudioTransitionSettings::peak_factor,
 &AudioTransitionSettings::minimum_onsets,
 &AudioTransitionSettings::confident_onsets,
 &AudioTransitionSettings::interval_penalty,
 &AudioTransitionSettings::bass_start,
 &AudioTransitionSettings::bass_end,
 &AudioTransitionSettings::limiter_release_ms,
 &AudioTransitionSettings::peak_limit, &AudioTransitionSettings::maximum_track_fraction, &AudioTransitionSettings::beat_group };
const auto edge_members=std::array{&AudioEdgeSettings::rms_db,&AudioEdgeSettings::peak_db,&AudioEdgeSettings::window_ms,&AudioEdgeSettings::minimum_ms,&AudioEdgeSettings::guard_ms,&AudioEdgeSettings::scan_ms};
const LyraFieldRuleV3 edge_rules[]={
 {LYRA_TRANSITION_TRIM_SILENCE,LYRA_FIELD_I64},{LYRA_TRANSITION_SILENCE_RMS_DB,LYRA_FIELD_F64},
 {LYRA_TRANSITION_SILENCE_PEAK_DB,LYRA_FIELD_F64},{LYRA_TRANSITION_SILENCE_WINDOW_MS,LYRA_FIELD_F64},
 {LYRA_TRANSITION_SILENCE_MINIMUM_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_SILENCE_GUARD_MS,LYRA_FIELD_F64},
 {LYRA_TRANSITION_SILENCE_SCAN_MS,LYRA_FIELD_F64}};
const auto adaptive_members=std::array{&AdaptiveTransitionSettings::confidence,&AdaptiveTransitionSettings::split_hz,&AdaptiveTransitionSettings::smooth_ms,&AdaptiveTransitionSettings::tolerance_ms,&AdaptiveTransitionSettings::phase_wait_ms,&AdaptiveTransitionSettings::echo_wet,&AdaptiveTransitionSettings::echo_feedback,&AdaptiveTransitionSettings::echo_beats,&AdaptiveTransitionSettings::echo_max_ms,&AdaptiveTransitionSettings::echo_low_hz,&AdaptiveTransitionSettings::filter_min_hz,&AdaptiveTransitionSettings::filter_max_hz,&AdaptiveTransitionSettings::gain_min,&AdaptiveTransitionSettings::gain_max,&AdaptiveTransitionSettings::mid_start,&AdaptiveTransitionSettings::mid_end,&AdaptiveTransitionSettings::loop_ms,&AdaptiveTransitionSettings::bridge_max_ms,&AdaptiveTransitionSettings::bridge_fade_ms,&AdaptiveTransitionSettings::loop_cross_ms,&AdaptiveTransitionSettings::cache_entries};
const LyraFieldRuleV3 adaptive_rules[]={{LYRA_TRANSITION_ADAPTIVE_ENABLED,LYRA_FIELD_I64},{LYRA_TRANSITION_ADAPTIVE_CONFIDENCE,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_SPLIT_HZ,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_SMOOTH_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_TOLERANCE_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_PHASE_WAIT_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_ECHO_WET,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_ECHO_FEEDBACK,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_ECHO_BEATS,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_ECHO_MAX_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_ECHO_LOW_HZ,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_FILTER_MIN_HZ,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_FILTER_MAX_HZ,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_GAIN_MIN,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_GAIN_MAX,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_MID_START,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_MID_END,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_LOOP_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_BRIDGE_MAX_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_BRIDGE_FADE_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_LOOP_CROSS_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_ADAPTIVE_CACHE_ENTRIES,LYRA_FIELD_F64}};
const auto full_members=std::array{&FullTrackSettings::hop_ms,&FullTrackSettings::novelty_ms,&FullTrackSettings::novelty_threshold,&FullTrackSettings::phrase_min_ms,&FullTrackSettings::max_ms,&FullTrackSettings::cache_mb,&FullTrackSettings::queue_limit,&FullTrackSettings::idle_ms,&FullTrackSettings::vocal_threshold,&FullTrackSettings::vocal_floor_db,&FullTrackSettings::vocal_smooth_ms,&FullTrackSettings::vocal_guard_ms,&FullTrackSettings::vocal_fade_ms,&FullTrackSettings::wait_ms,&FullTrackSettings::intro_ms,&FullTrackSettings::stem_window_ms,&FullTrackSettings::confidence,&FullTrackSettings::chroma_low_hz,&FullTrackSettings::chroma_high_hz,&FullTrackSettings::worker_nice};
const LyraFieldRuleV3 full_rules[]={{LYRA_TRANSITION_FULL_ENABLED,LYRA_FIELD_I64},{LYRA_TRANSITION_FULL_LIBRARY,LYRA_FIELD_I64},{LYRA_TRANSITION_FULL_HOP_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_NOVELTY_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_NOVELTY_THRESHOLD,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_PHRASE_MIN_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_MAX_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_CACHE_MB,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_QUEUE_LIMIT,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_IDLE_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_VOCAL_THRESHOLD,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_VOCAL_FLOOR_DB,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_VOCAL_SMOOTH_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_VOCAL_GUARD_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_VOCAL_FADE_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_WAIT_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_INTRO_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_STEM_WINDOW_MS,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_CONFIDENCE,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_CHROMA_LOW_HZ,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_CHROMA_HIGH_HZ,LYRA_FIELD_F64},{LYRA_TRANSITION_FULL_WORKER_NICE,LYRA_FIELD_F64}};
const LyraFieldRuleV3 rules[]={
 {LYRA_TRANSITION_ENABLED,LYRA_FIELD_I64},
 {LYRA_TRANSITION_AUTOMATIC_MS,LYRA_FIELD_F64},
 {LYRA_TRANSITION_MANUAL_MS,LYRA_FIELD_F64},
 {LYRA_TRANSITION_BRIDGE_MS,LYRA_FIELD_F64},
 {LYRA_TRANSITION_ANALYSIS_MS,LYRA_FIELD_F64},
 {LYRA_TRANSITION_CONFIDENCE,LYRA_FIELD_F64},
 {LYRA_TRANSITION_MAX_RATE_DELTA,LYRA_FIELD_F64},
 {LYRA_TRANSITION_BASS_HZ,LYRA_FIELD_F64},
 {LYRA_TRANSITION_RETURN_MS,LYRA_FIELD_F64},
 {LYRA_TRANSITION_FEATURE_HZ,LYRA_FIELD_F64},
 {LYRA_TRANSITION_BPM_MIN,LYRA_FIELD_F64},
 {LYRA_TRANSITION_BPM_MAX,LYRA_FIELD_F64},
 {LYRA_TRANSITION_ONSET_FLOOR,LYRA_FIELD_F64},
 {LYRA_TRANSITION_PEAK_FACTOR,LYRA_FIELD_F64},
 {LYRA_TRANSITION_MINIMUM_ONSETS,LYRA_FIELD_F64},
 {LYRA_TRANSITION_CONFIDENT_ONSETS,LYRA_FIELD_F64},
 {LYRA_TRANSITION_INTERVAL_PENALTY,LYRA_FIELD_F64},
 {LYRA_TRANSITION_BASS_START,LYRA_FIELD_F64},
 {LYRA_TRANSITION_BASS_END,LYRA_FIELD_F64},
 {LYRA_TRANSITION_LIMITER_RELEASE_MS,LYRA_FIELD_F64},
 {LYRA_TRANSITION_PEAK_LIMIT,LYRA_FIELD_F64}, {LYRA_TRANSITION_MAXIMUM_TRACK_FRACTION,LYRA_FIELD_F64}, {LYRA_TRANSITION_BEAT_GROUP,LYRA_FIELD_F64} };
bool valid(const LyraAudioTransitionV3 *api){return api && api->size==sizeof(*api) && api->version==LYRA_INTERFACE_VERSION && api->request;}
}
AudioTransitionControl::AudioTransitionControl(){api_={sizeof(api_),LYRA_INTERFACE_VERSION,this,request};}
AudioTransitionControl::~AudioTransitionControl(){for(auto &job:jobs_)if(job.fd>=0)::close(job.fd);}
void AudioTransitionControl::enqueue_analysis(int fd,int64_t id,int64_t duration){
 std::lock_guard<std::mutex> lock(mutex_);if(!settings_.full.enabled || cache_directory_.empty() || fd<0)return;
 for(auto it=jobs_.begin();it!=jobs_.end();++it)if(it->id==id){::close(it->fd);jobs_.erase(it);break;}
 int copy=dup(fd);if(copy<0)return;
 while(jobs_.size()>=std::min<size_t>(2,size_t(settings_.full.queue_limit))){::close(jobs_.back().fd);jobs_.pop_back();}
 jobs_.push_front({copy,id,duration});++analysis_revision_;changed_.notify_all();
}
bool AudioTransitionControl::pop_analysis(AnalysisJob *job){std::lock_guard<std::mutex> lock(mutex_);if(jobs_.empty())return false;*job=jobs_.front();jobs_.pop_front();return true;}
void AudioTransitionControl::wait_analysis(const std::atomic<bool> &stop,uint64_t *revision){std::unique_lock<std::mutex> lock(mutex_);changed_.wait(lock,[&]{return stop.load() || !jobs_.empty() || analysis_revision_!=*revision;});*revision=analysis_revision_;}
int AudioTransitionControl::request(void *user,const LyraMessageV3 *message,LyraReplyV3 *reply){
 if(!reply || reply->size!=sizeof(*reply))return LYRA_MESSAGE_INVALID;
 reply->field_count=0;reply->status=LYRA_MESSAGE_INVALID;
 if(!message || message->size!=sizeof(*message))return reply->status;
 auto &self=*static_cast<AudioTransitionControl*>(user);
 if(message->operation==LYRA_TRANSITION_STEM_ENDPOINT){
  LyraFieldRuleV3 rule{LYRA_TRANSITION_STEM_API,LYRA_FIELD_HANDLE};if(lyra_message_fields(message,&rule,1)<0 || message->field_count!=1)return reply->status;
  const auto &f=message->fields[0];const LyraStemAnalysisV3 *api=nullptr;if(f.tag!=rule.tag || !f.data || f.bytes!=sizeof(api))return reply->status;memcpy(&api,f.data,sizeof(api));
  if(api && (api->size!=sizeof(*api) || api->version!=LYRA_INTERFACE_VERSION || !api->request))return reply->status;
  {std::lock_guard<std::mutex> lock(self.mutex_);self.stem_=api;++self.analysis_revision_;self.changed_.notify_all();}return reply->status=LYRA_MESSAGE_OK;
 }
 if(message->operation==LYRA_TRANSITION_ANALYZE_SOURCE){
  LyraFieldRuleV3 rules[]={{LYRA_TRANSITION_SOURCE_FD,LYRA_FIELD_I64},{LYRA_TRANSITION_SOURCE_ID,LYRA_FIELD_I64},{LYRA_TRANSITION_SOURCE_DURATION_US,LYRA_FIELD_I64}};
  if(lyra_message_fields(message,rules,std::size(rules))<0 || message->field_count!=std::size(rules))return reply->status;
  int64_t values[std::size(rules)]{};bool seen[std::size(rules)]{};
  for(uint32_t i=0;i<message->field_count;++i){const auto &f=message->fields[i];if(f.tag<LYRA_TRANSITION_SOURCE_FD || f.tag>LYRA_TRANSITION_SOURCE_DURATION_US || !f.data || f.bytes!=sizeof(int64_t))return reply->status;auto n=f.tag-LYRA_TRANSITION_SOURCE_FD;if(seen[n])return reply->status;seen[n]=true;memcpy(&values[n],f.data,sizeof(int64_t));}
  if(values[0]<0 || values[0]>INT32_MAX || values[1]<0 || values[2]<0)return reply->status;self.enqueue_analysis(int(values[0]),values[1],values[2]);return reply->status=LYRA_MESSAGE_OK;
 }
 if(message->operation==LYRA_TRANSITION_CONFIGURE_FULL){
  if(lyra_message_fields(message,full_rules,std::size(full_rules))<0)return reply->status;
  FullTrackSettings full;bool seen[std::size(full_rules)]{};
  for(uint32_t i=0;i<message->field_count;++i){const auto &f=message->fields[i];if(f.tag<LYRA_TRANSITION_FULL_ENABLED || f.tag>LYRA_TRANSITION_FULL_WORKER_NICE)continue;auto n=f.tag-LYRA_TRANSITION_FULL_ENABLED;if(seen[n] || !f.data)return reply->status;seen[n]=true;
   if(n<2){int64_t value;if(f.bytes!=sizeof(value))return reply->status;memcpy(&value,f.data,sizeof(value));if(value!=0 && value!=1)return reply->status;if(n==0)full.enabled=value!=0;else full.library=value!=0;}
   else{double value;if(f.bytes!=sizeof(value))return reply->status;memcpy(&value,f.data,sizeof(value));full.*full_members[n-2]=value;}}
  for(bool present:seen)if(!present)return reply->status;if(!full_track_valid(full))return reply->status;
  {std::lock_guard<std::mutex> lock(self.mutex_);self.settings_.full=full;++self.analysis_revision_;self.changed_.notify_all();}return reply->status=LYRA_MESSAGE_OK;
 }
 if(message->operation==LYRA_TRANSITION_CACHE_PATH){
  LyraFieldRuleV3 rule{LYRA_TRANSITION_MAP_DIRECTORY,LYRA_FIELD_UTF8};
  if(lyra_message_fields(message,&rule,1)<0 || message->field_count!=1)return reply->status;
  const auto &f=message->fields[0];if(f.tag!=rule.tag || !f.data || f.bytes>4096 || std::memchr(f.data,0,f.bytes))return reply->status;
  {std::lock_guard<std::mutex> lock(self.mutex_);self.cache_directory_.assign(static_cast<const char*>(f.data),f.bytes);++self.analysis_revision_;self.changed_.notify_all();}
  return reply->status=LYRA_MESSAGE_OK;
 }
 if(message->operation==LYRA_TRANSITION_CONFIGURE_ADAPTIVE){
  if(lyra_message_fields(message,adaptive_rules,std::size(adaptive_rules))<0)return reply->status;
  AdaptiveTransitionSettings adaptive;bool seen[std::size(adaptive_rules)]{};
  for(uint32_t i=0;i<message->field_count;++i){const auto &f=message->fields[i];
   if(f.tag<LYRA_TRANSITION_ADAPTIVE_ENABLED || f.tag>LYRA_TRANSITION_ADAPTIVE_CACHE_ENTRIES)continue;
   auto n=f.tag-LYRA_TRANSITION_ADAPTIVE_ENABLED;if(seen[n] || !f.data)return reply->status;seen[n]=true;
   if(n==0){int64_t v;if(f.bytes!=sizeof(v))return reply->status;memcpy(&v,f.data,sizeof(v));if(v!=0 && v!=1)return reply->status;adaptive.enabled=v!=0;}
   else{double v;if(f.bytes!=sizeof(v))return reply->status;memcpy(&v,f.data,sizeof(v));adaptive.*adaptive_members[n-1]=v;}
  }
  for(bool present:seen)if(!present)return reply->status;if(!adaptive_transition_valid(adaptive))return reply->status;
  {std::lock_guard<std::mutex> lock(self.mutex_);self.settings_.adaptive=adaptive;}
  return reply->status=LYRA_MESSAGE_OK;
 }
 if(message->operation==LYRA_TRANSITION_CONFIGURE_EDGES){
  if(lyra_message_fields(message,edge_rules,std::size(edge_rules))<0)return reply->status;
  AudioEdgeSettings edges;bool seen[std::size(edge_rules)]{};
  for(uint32_t i=0;i<message->field_count;++i){const auto &f=message->fields[i];
   if(f.tag<LYRA_TRANSITION_TRIM_SILENCE || f.tag>LYRA_TRANSITION_SILENCE_SCAN_MS)continue;
   auto n=f.tag-LYRA_TRANSITION_TRIM_SILENCE;if(seen[n] || !f.data)return reply->status;seen[n]=true;
   if(n==0){int64_t value;if(f.bytes!=sizeof(value))return reply->status;memcpy(&value,f.data,sizeof(value));if(value!=0 && value!=1)return reply->status;edges.enabled=value!=0;}
   else{double value;if(f.bytes!=sizeof(value))return reply->status;memcpy(&value,f.data,sizeof(value));edges.*edge_members[n-1]=value;}
  }
  for(bool present:seen)if(!present)return reply->status;
  if(!audio_edges_valid(edges))return reply->status;
  {std::lock_guard<std::mutex> lock(self.mutex_);self.settings_.edges=edges;}
  return reply->status=LYRA_MESSAGE_OK;
 }
 if(message->operation==LYRA_TRANSITION_QUERY_EDGES){
  if(lyra_message_fields(message,nullptr,0)<0)return reply->status;
  if(reply->capacity<2 || !reply->fields)return reply->status=LYRA_MESSAGE_CAPACITY;
  std::lock_guard<std::mutex> lock(self.mutex_);self.reply_generation_=self.edge_generation_;self.reply_end_=self.settings_.edges.enabled?self.edge_end_:0;
  reply->fields[0]={LYRA_TRANSITION_EDGE_GENERATION,LYRA_FIELD_HANDLE,0,sizeof(self.reply_generation_),&self.reply_generation_};
  reply->fields[1]={LYRA_TRANSITION_AUDIBLE_END_US,LYRA_FIELD_I64,0,sizeof(self.reply_end_),&self.reply_end_};
  reply->field_count=2;return reply->status=LYRA_MESSAGE_OK;
 }
 if(message->operation==LYRA_TRANSITION_QUERY){
  if(lyra_message_fields(message,nullptr,0)<0)return reply->status;
  if(reply->capacity<2 || !reply->fields)return reply->status=LYRA_MESSAGE_CAPACITY;
  std::lock_guard<std::mutex> lock(self.mutex_);
  self.reply_enabled_=self.settings_.enabled;self.reply_ms_=self.settings_.automatic_ms;
  reply->fields[0]={LYRA_TRANSITION_ENABLED,LYRA_FIELD_I64,0,sizeof(self.reply_enabled_),&self.reply_enabled_};
  reply->fields[1]={LYRA_TRANSITION_AUTOMATIC_MS,LYRA_FIELD_F64,0,sizeof(self.reply_ms_),&self.reply_ms_};
  reply->field_count=2;return reply->status=LYRA_MESSAGE_OK;
 }
 if(message->operation!=LYRA_TRANSITION_CONFIGURE)return reply->status=LYRA_MESSAGE_UNSUPPORTED;
 if(lyra_message_fields(message,rules,sizeof(rules)/sizeof(rules[0]))<0)return reply->status;
 AudioTransitionSettings s;
 bool seen[sizeof(rules)/sizeof(rules[0])]{};
 for(uint32_t i=0;i<message->field_count;++i){const auto &f=message->fields[i];
  if(f.tag<LYRA_TRANSITION_ENABLED || f.tag>LYRA_TRANSITION_BEAT_GROUP)continue;
  auto n=f.tag-LYRA_TRANSITION_ENABLED;if(seen[n] || !f.data)return reply->status;seen[n]=true;
  if(n==0){int64_t value=0;if(f.bytes!=sizeof(value))return reply->status;memcpy(&value,f.data,sizeof(value));if(value!=0 && value!=1)return reply->status;s.enabled=value!=0;}
  else{double value=0;if(f.bytes!=sizeof(value))return reply->status;memcpy(&value,f.data,sizeof(value));if(!std::isfinite(value)||value<=0)return reply->status;s.*members[n-1]=value;}
 }
 for(bool present:seen)if(!present)return reply->status;
 if(!audio_transition_valid(s))return reply->status;
 {std::lock_guard<std::mutex> lock(self.mutex_);self.settings_=s;}
 return reply->status=LYRA_MESSAGE_OK;
}
int configure_transition(const LyraAudioTransitionV3 *api,const AudioTransitionSettings &s){
 if(!valid(api))return LYRA_MESSAGE_UNSUPPORTED;
 if(!audio_transition_valid(s))return LYRA_MESSAGE_INVALID;
 int64_t enabled=s.enabled;std::array<double,std::size(members)> values{};
 for(size_t i=0;i<values.size();++i)values[i]=s.*members[i];
 LyraFieldV3 fields[sizeof(rules)/sizeof(rules[0])]{};
 fields[0]={LYRA_TRANSITION_ENABLED,LYRA_FIELD_I64,LYRA_FIELD_REQUIRED,sizeof(enabled),&enabled};
 for(size_t i=1;i<std::size(fields);++i)fields[i]={rules[i].tag,LYRA_FIELD_F64,LYRA_FIELD_REQUIRED,sizeof(double),&values[i-1]};
 LyraMessageV3 message{sizeof(message),uint32_t(std::size(fields)),LYRA_TRANSITION_CONFIGURE,fields};LyraReplyV3 reply{sizeof(reply),0,0,0,nullptr};
 int result=api->request(api->user,&message,&reply);if(result!=0)return result;
 int64_t trim=s.edges.enabled;std::array<double,std::size(edge_members)> edge_values{};
 LyraFieldV3 edge_fields[std::size(edge_rules)]{};
 edge_fields[0]={LYRA_TRANSITION_TRIM_SILENCE,LYRA_FIELD_I64,LYRA_FIELD_REQUIRED,sizeof(trim),&trim};
 for(size_t i=0;i<edge_values.size();++i){edge_values[i]=s.edges.*edge_members[i];edge_fields[i+1]={edge_rules[i+1].tag,LYRA_FIELD_F64,LYRA_FIELD_REQUIRED,sizeof(double),&edge_values[i]};}
 message={sizeof(message),uint32_t(std::size(edge_fields)),LYRA_TRANSITION_CONFIGURE_EDGES,edge_fields};reply={sizeof(reply),0,0,0,nullptr};
 result=api->request(api->user,&message,&reply);if(result!=0 && result!=LYRA_MESSAGE_UNSUPPORTED)return result;
 int64_t adaptive_enabled=s.adaptive.enabled;std::array<double,std::size(adaptive_members)> adaptive_values{};LyraFieldV3 adaptive_fields[std::size(adaptive_rules)]{};
 adaptive_fields[0]={LYRA_TRANSITION_ADAPTIVE_ENABLED,LYRA_FIELD_I64,LYRA_FIELD_REQUIRED,sizeof(adaptive_enabled),&adaptive_enabled};
 for(size_t i=0;i<adaptive_values.size();++i){adaptive_values[i]=s.adaptive.*adaptive_members[i];adaptive_fields[i+1]={adaptive_rules[i+1].tag,LYRA_FIELD_F64,LYRA_FIELD_REQUIRED,sizeof(double),&adaptive_values[i]};}
 message={sizeof(message),uint32_t(std::size(adaptive_fields)),LYRA_TRANSITION_CONFIGURE_ADAPTIVE,adaptive_fields};reply={sizeof(reply),0,0,0,nullptr};
 result=api->request(api->user,&message,&reply);if(result!=0 && result!=LYRA_MESSAGE_UNSUPPORTED)return result;
 int64_t full_enabled=s.full.enabled,full_library=s.full.library;std::array<double,std::size(full_members)> full_values{};LyraFieldV3 full_fields[std::size(full_rules)]{};
 full_fields[0]={LYRA_TRANSITION_FULL_ENABLED,LYRA_FIELD_I64,LYRA_FIELD_REQUIRED,sizeof(full_enabled),&full_enabled};full_fields[1]={LYRA_TRANSITION_FULL_LIBRARY,LYRA_FIELD_I64,LYRA_FIELD_REQUIRED,sizeof(full_library),&full_library};
 for(size_t i=0;i<full_values.size();++i){full_values[i]=s.full.*full_members[i];full_fields[i+2]={full_rules[i+2].tag,LYRA_FIELD_F64,LYRA_FIELD_REQUIRED,sizeof(double),&full_values[i]};}
 message={sizeof(message),uint32_t(std::size(full_fields)),LYRA_TRANSITION_CONFIGURE_FULL,full_fields};reply={sizeof(reply),0,0,0,nullptr};result=api->request(api->user,&message,&reply);return result==LYRA_MESSAGE_UNSUPPORTED?0:result;
}
double automatic_transition_ms(const LyraAudioTransitionV3 *api){
 if(!valid(api))return 0;
 LyraMessageV3 message{sizeof(message),0,LYRA_TRANSITION_QUERY,nullptr};LyraFieldV3 fields[2]{};LyraReplyV3 reply{sizeof(reply),2,0,0,fields};
 if(api->request(api->user,&message,&reply)!=0 || reply.status!=LYRA_MESSAGE_OK || reply.size!=sizeof(reply) ||
    reply.capacity!=std::size(fields) || reply.fields!=fields || reply.field_count>std::size(fields))return 0;
 int64_t enabled=0;double ms=0;
 for(uint32_t i=0;i<reply.field_count;++i){const auto &f=fields[i];if(!f.data)continue;
  if(f.tag==LYRA_TRANSITION_ENABLED && f.type==LYRA_FIELD_I64 && f.bytes==sizeof(enabled))memcpy(&enabled,f.data,sizeof(enabled));
  if(f.tag==LYRA_TRANSITION_AUTOMATIC_MS && f.type==LYRA_FIELD_F64 && f.bytes==sizeof(ms))memcpy(&ms,f.data,sizeof(ms));
 }
 return enabled==1 && std::isfinite(ms) && ms>0 && ms<=8000?ms:0;
}

int64_t transition_audible_end(const LyraAudioTransitionV3 *api,uint64_t generation,int64_t fallback){
 if(!valid(api))return fallback;
 LyraMessageV3 message{sizeof(message),0,LYRA_TRANSITION_QUERY_EDGES,nullptr};LyraFieldV3 fields[2]{};LyraReplyV3 reply{sizeof(reply),2,0,0,fields};
 if(api->request(api->user,&message,&reply)!=0 || reply.status!=0 || reply.size!=sizeof(reply) || reply.capacity!=std::size(fields) || reply.fields!=fields || reply.field_count!=std::size(fields))return fallback;
 uint64_t actual=0;int64_t end=0;
 for(const auto &f:fields){if(!f.data)return fallback;
  if(f.tag==LYRA_TRANSITION_EDGE_GENERATION && f.type==LYRA_FIELD_HANDLE && f.bytes==sizeof(actual))memcpy(&actual,f.data,sizeof(actual));
  if(f.tag==LYRA_TRANSITION_AUDIBLE_END_US && f.type==LYRA_FIELD_I64 && f.bytes==sizeof(end))memcpy(&end,f.data,sizeof(end));}
 return actual==generation && end>0?std::min(fallback,end):fallback;
}

int configure_transition_cache(const LyraAudioTransitionV3 *api,const std::string &path){
 if(!valid(api))return LYRA_MESSAGE_UNSUPPORTED;if(path.size()>4096)return LYRA_MESSAGE_INVALID;LyraFieldV3 field{LYRA_TRANSITION_MAP_DIRECTORY,LYRA_FIELD_UTF8,LYRA_FIELD_REQUIRED,uint32_t(path.size()),path.data()};
 LyraMessageV3 message{sizeof(message),1,LYRA_TRANSITION_CACHE_PATH,&field};LyraReplyV3 reply{sizeof(reply),0,0,0,nullptr};return api->request(api->user,&message,&reply);
}

int bind_transition_stems(const LyraAudioTransitionV3 *api,const LyraStemAnalysisV3 *stem){
 if(!valid(api))return LYRA_MESSAGE_UNSUPPORTED;LyraFieldV3 field{LYRA_TRANSITION_STEM_API,LYRA_FIELD_HANDLE,LYRA_FIELD_REQUIRED,sizeof(stem),&stem};LyraMessageV3 message{sizeof(message),1,LYRA_TRANSITION_STEM_ENDPOINT,&field};LyraReplyV3 reply{sizeof(reply),0,0,0,nullptr};return api->request(api->user,&message,&reply);
}
