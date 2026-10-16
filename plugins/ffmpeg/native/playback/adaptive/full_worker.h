#pragma once
#include "playback/adaptive/full_map.h"
#include "playback/transition_control.h"
#include "platform/log.h"
#include <pthread.h>
#include <sys/resource.h>
#include <chrono>
#include <thread>
namespace lyra::adaptive {
inline bool stem_call(const LyraStemAnalysisV3 *api,uint64_t operation,const LyraFieldV3 *fields,uint32_t count,LyraFieldV3 *out,uint32_t capacity,uint32_t *returned){
 if(!api || api->size!=sizeof(*api) || api->version!=LYRA_INTERFACE_VERSION || !api->request)return false;
 LyraMessageV3 request{sizeof(request),count,operation,fields};LyraReplyV3 reply{sizeof(reply),capacity,0,0,out};int result=api->request(api->user,&request,&reply);
 if(result || reply.status!=LYRA_MESSAGE_OK || reply.size!=sizeof(reply) || reply.fields!=out || reply.capacity!=capacity || reply.field_count>capacity)return false;if(returned)*returned=reply.field_count;return true;
}
inline void trim_full_cache(const std::string &root,uint64_t keep,const FullTrackSettings &settings,int64_t reserve=0){
 struct Entry{uint64_t key;time_t time;int64_t bytes;};std::vector<Entry> entries;int64_t bytes=0;DIR *dir=opendir(root.c_str());if(!dir)return;
 while(auto *e=readdir(dir)){std::string name=e->d_name;if(name.size()!=28 || name.compare(0,5,"full-") || name.compare(21,7,".vocals"))continue;unsigned long long key=0;int used=0;if(std::sscanf(name.c_str(),"full-%16llx.vocals%n",&key,&used)!=1 || used!=int(name.size()))continue;struct stat st{};auto path=root+"/"+name;if(stat(path.c_str(),&st))continue;bytes+=st.st_size;entries.push_back({uint64_t(key),st.st_mtime,st.st_size});}
 closedir(dir);std::sort(entries.begin(),entries.end(),[](const Entry &a,const Entry &b){return a.time<b.time;});
 for(const auto &e:entries){if(bytes+reserve<=int64_t(settings.cache_mb*1048576))break;if(e.key==keep)continue;FullMap map;if(read_full_map(root,e.key,settings,&map)){map.header.stems&=1;write_full_map(root,map,settings);}auto path=full_path(root,e.key,"vocals");if(!unlink(path.c_str()))bytes-=e.bytes;}
}
template<class Decoder> class FullTrackWorker {
 AudioTransitionControl &control_;std::atomic<bool> stopping_{false};pthread_t worker_{};bool live_=false;
 std::vector<int64_t> library_;size_t library_at_=0;const LyraStemAnalysisV3 *library_endpoint_=nullptr;std::string library_root_;bool library_loaded_=false;
 static void *entry(void *self){static_cast<FullTrackWorker*>(self)->run();return nullptr;}
 bool library_next(const LyraStemAnalysisV3 *api,const std::string &root,const FullTrackSettings &settings,AudioTransitionControl::AnalysisJob *job){
  if(!api || !settings.library)return false;
  if(api!=library_endpoint_ || root!=library_root_){library_.clear();library_at_=0;library_loaded_=false;library_endpoint_=api;library_root_=root;}
  if(!library_loaded_){int64_t maximum=int64_t(settings.queue_limit);LyraFieldV3 field{LYRA_STEM_LIBRARY_LIMIT,LYRA_FIELD_I64,LYRA_FIELD_REQUIRED,sizeof(maximum),&maximum},out{};uint32_t count=0;
   if(!stem_call(api,LYRA_STEM_LIBRARY,&field,1,&out,1,&count) || count!=1 || out.tag!=LYRA_STEM_LIBRARY_ROWS || out.type!=LYRA_FIELD_BYTES || out.bytes>size_t(maximum)*2*sizeof(int64_t) || out.bytes%(2*sizeof(int64_t)) || (out.bytes && !out.data))return false;
   library_.resize(out.bytes/sizeof(int64_t));if(out.bytes)memcpy(library_.data(),out.data,out.bytes);library_loaded_=!library_.empty();library_at_=0;
  }
  while(library_at_+1<library_.size() && !stopping_){int64_t id=library_[library_at_++],duration=library_[library_at_++];if(id<0 || duration<=0 || duration>settings.max_ms)continue;LyraFieldV3 field{LYRA_STEM_TRACK_ID,LYRA_FIELD_I64,LYRA_FIELD_REQUIRED,sizeof(id),&id},out{};uint32_t count=0;
   if(!stem_call(api,LYRA_STEM_OPEN_SOURCE,&field,1,&out,1,&count) || count!=1 || out.tag!=LYRA_STEM_FD || out.type!=LYRA_FIELD_I64 || out.bytes!=sizeof(int64_t) || !out.data)continue;
   int64_t fd=-1;memcpy(&fd,out.data,sizeof(fd));if(fd<0 || fd>INT32_MAX)continue;*job={int(fd),id,duration*1000};return true;
  }return false;
 }
 void analyze(AudioTransitionControl::AnalysisJob job,const AudioTransitionSettings &settings,const std::string &root,const LyraStemAnalysisV3 *api,bool priority){
  uint64_t key=full_key(job.fd);FullMap cached;bool exists=read_full_map(root,key,settings.full,&cached);
  if(!key || (exists && (!api || ((cached.header.stems&1) && (!priority || (cached.header.stems&2)))))){::close(job.fd);return;}
  Decoder decoder;decoder.fd=job.fd;if(!decoder.open() || decoder.duration_us>settings.full.max_ms*1000)return;
  mkdir(root.c_str(),0700);FullMap map;map.header.key=key;map.header.configuration=full_signature(settings.full);map.header.hop_ms=settings.full.hop_ms;map.header.stems=api?1:0;
  FullFeatures features(map,settings);bool neural=api!=nullptr,store=neural;FILE *file=nullptr;auto temporary=full_path(root,key,"vocals.tmp"),destination=full_path(root,key,"vocals");
  int64_t budget=int64_t(settings.full.cache_mb*1048576),expected=decoder.duration_us>0?decoder.duration_us*LYRA_STEM_RATE/1000000*int64_t(sizeof(int16_t))*2:budget;
  if(expected>budget/2)store=false;
  if(store){trim_full_cache(root,key,settings.full,expected);file=std::fopen(temporary.c_str(),"wb");if(!file)store=false;}
  std::vector<int16_t> pending(size_t(LYRA_STEM_CONTEXT)*2,0),read(size_t(LYRA_STEM_INPUT_FRAMES)*2),vocal_pcm(size_t(LYRA_STEM_OUTPUT_HOP)*2);
  std::vector<float> input(size_t(LYRA_STEM_INPUT_FRAMES)*2),aligned(size_t(LYRA_STEM_OUTPUT_HOP)*2),tail(size_t(LYRA_STEM_FFT)*2);bool have_tail=false;pending.reserve(size_t(LYRA_STEM_INPUT_FRAMES+LYRA_STEM_OUTPUT_HOP)*2);
  std::vector<int16_t> beat_pcm;beat_pcm.reserve(size_t(LYRA_STEM_RATE*settings.analysis_ms/1000)*2+LYRA_STEM_OUTPUT_HOP*2);size_t beat_row=0;TransitionBeat last_beat;
  auto finish_beat=[&]{if(beat_pcm.empty())return;auto beat=lyra::adaptive::analyze(beat_pcm.data(),beat_pcm.size()/2,LYRA_STEM_RATE,settings).rhythm();last_beat=beat;for(size_t i=beat_row;i<map.rows.size();++i){map.rows[i].bpm=float(beat.bpm);map.rows[i].beat_confidence=float(beat.confidence);}if(beat.confidence>map.header.confidence){map.header.bpm=beat.bpm;map.header.confidence=beat.confidence;}beat_row=map.rows.size();beat_pcm.clear();};
  int64_t decoded=0,written=0;bool eof=false,ok=true;auto started=std::chrono::steady_clock::now();
  while(!stopping_ && !eof){
   while(control_.analysis_busy.load() && !stopping_)std::this_thread::sleep_for(std::chrono::milliseconds(int(settings.full.idle_ms)));
   while(pending.size()<size_t(LYRA_STEM_INPUT_FRAMES)*2 && !eof && !stopping_){int want=int(std::min<size_t>((size_t(LYRA_STEM_INPUT_FRAMES)*2-pending.size())/2,LYRA_STEM_OUTPUT_HOP));int got=decoder.pull(read.data(),want,LYRA_STEM_RATE,stopping_);if(got<0){ok=false;eof=true;break;}if(!got){eof=true;break;}pending.insert(pending.end(),read.begin(),read.begin()+got*2);decoded+=got;if(decoded>int64_t(LYRA_STEM_RATE*settings.full.max_ms/1000)){ok=false;eof=true;break;}}
   if(!ok || stopping_)break;
   if(written>=decoded && eof)break;
   bool source_end=eof;while(written<decoded && !stopping_){
    if(pending.size()<size_t(LYRA_STEM_INPUT_FRAMES)*2){if(!source_end)break;pending.resize(size_t(LYRA_STEM_INPUT_FRAMES)*2,0);}
    int count=int(std::min<int64_t>(LYRA_STEM_OUTPUT_HOP,decoded-written));if(count<=0)break;
    for(size_t i=0;i<input.size();++i)input[i]=pending[i]/32768.f;
    const float *vocal=nullptr;LyraFieldV3 result{};uint32_t returned=0;
    if(neural){LyraFieldV3 field{LYRA_STEM_PCM,LYRA_FIELD_BYTES,LYRA_FIELD_REQUIRED,uint32_t(input.size()*sizeof(float)),input.data()};
     bool ready=stem_call(api,LYRA_STEM_PROCESS,&field,1,&result,1,&returned) && returned==1 && result.tag==LYRA_STEM_VOCALS && result.type==LYRA_FIELD_BYTES && result.bytes==input.size()*sizeof(float) && result.data;
     if(ready){vocal=static_cast<const float*>(result.data)+LYRA_STEM_CONTEXT*2;for(int i=0;i<(LYRA_STEM_OUTPUT_HOP+LYRA_STEM_FFT)*2;++i)if(!std::isfinite(vocal[i]) || std::abs(vocal[i])>2){ready=false;break;}}
     if(!ready){neural=false;map.header.stems=0;vocal=nullptr;if(file){std::fclose(file);file=nullptr;unlink(temporary.c_str());}store=false;lyra_log(LyraLogError,"Lyra","full analysis neural fallback track=%lld",(long long)job.id);}
    }
    if(vocal){
     std::copy(vocal,vocal+count*2,aligned.begin());
     if(have_tail)for(int i=0;i<std::min(count,int(LYRA_STEM_FFT));++i){float t=float(i)/LYRA_STEM_FFT;t=t*t*(3-2*t);for(int c=0;c<2;++c)aligned[i*2+c]=tail[i*2+c]*(1-t)+aligned[i*2+c]*t;}
     std::copy(vocal+LYRA_STEM_OUTPUT_HOP*2,vocal+(LYRA_STEM_OUTPUT_HOP+LYRA_STEM_FFT)*2,tail.begin());have_tail=true;vocal=aligned.data();
    }
    if(map.rows.empty())map.header.start_us=std::max<int64_t>(0,decoder.first_media_us);
    features.feed(pending.data()+LYRA_STEM_CONTEXT*2,vocal,count);
    beat_pcm.insert(beat_pcm.end(),pending.begin()+LYRA_STEM_CONTEXT*2,pending.begin()+(LYRA_STEM_CONTEXT+count)*2);if(beat_pcm.size()/2>=size_t(LYRA_STEM_RATE*settings.analysis_ms/1000))finish_beat();
    if(file && vocal){for(int i=0;i<count*2;++i)vocal_pcm[i]=int16_t(std::clamp(vocal[i]*32767.f,-32768.f,32767.f));if(std::fwrite(vocal_pcm.data(),sizeof(int16_t)*2,size_t(count),file)!=size_t(count)){std::fclose(file);file=nullptr;unlink(temporary.c_str());store=false;}}
    written+=count;pending.erase(pending.begin(),pending.begin()+count*2);
    if(written*int64_t(sizeof(int16_t))*2>budget/2 && file){std::fclose(file);file=nullptr;unlink(temporary.c_str());store=false;}
    if(!source_end)break;
    std::this_thread::sleep_for(std::chrono::milliseconds(int(settings.full.idle_ms)));
   }
  }
  if(file){bool closed=std::fclose(file)==0;if(!closed)store=false;}
  if(api)stem_call(api,LYRA_STEM_RELEASE,nullptr,0,nullptr,0,nullptr);
  if(!ok || stopping_ || written<=0){unlink(temporary.c_str());return;}
  map.header.frames=written;map.header.duration_us=decoder.duration_us>0?decoder.duration_us:map.header.start_us+written*1000000/LYRA_STEM_RATE;
  if(!neural){map.header.stems=0;for(auto &row:map.rows){row.voice=-1;row.vocal_rms=0;}}
  features.finish();finish_beat();if(!map.rows.empty() && map.rows.back().bpm==0){map.rows.back().bpm=float(last_beat.bpm);map.rows.back().beat_confidence=float(last_beat.confidence);}if(store && neural){if(rename(temporary.c_str(),destination.c_str()))store=false;else map.header.stems=3;}else unlink(temporary.c_str());
  if(!write_full_map(root,map,settings.full)){if(store)unlink(destination.c_str());return;}trim_full_cache(root,key,settings.full);
  lyra_log(LyraLogInfo,"Lyra","full analysis ready track=%lld duration_ms=%lld rows=%zu phrases=%zu vocals=%d stems=%d elapsed_ms=%lld",(long long)job.id,(long long)(written*1000/LYRA_STEM_RATE),map.rows.size(),map.phrases.size(),neural,store,(long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count());
 }
 void run(){uint64_t revision=0;for(;;){if(stopping_)return;auto settings=control_.settings();auto root=control_.cache_directory();auto *api=control_.stem_endpoint();AudioTransitionControl::AnalysisJob job;
   bool queued=control_.pop_analysis(&job),priority=queued;if(!queued && settings.full.enabled && !root.empty())queued=library_next(api,root,settings.full,&job);
   if(queued){if(settings.full.enabled && !root.empty()){setpriority(PRIO_PROCESS,0,int(settings.full.worker_nice));analyze(job,settings,root,api,priority);}else if(job.fd>=0)::close(job.fd);continue;}
   control_.wait_analysis(stopping_,&revision);
  }}
public:
 explicit FullTrackWorker(AudioTransitionControl &control):control_(control){live_=pthread_create(&worker_,nullptr,entry,this)==0;}
 ~FullTrackWorker(){shutdown();}
 void shutdown(){if(!live_)return;stopping_=true;control_.wake_analysis();pthread_join(worker_,nullptr);live_=false;}
};
}
