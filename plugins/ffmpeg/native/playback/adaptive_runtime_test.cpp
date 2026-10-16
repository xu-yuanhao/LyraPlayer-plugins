// Compile the real engine here to exercise its private emergency bridge without
// adding a test seam or new ABI to the production engine.
#include "playback/engine.cpp"
#include <cassert>
#include <cstdio>
void test_adaptive_runtime(const AudioTransitionSettings &s){
 EngineTransition runtime(s,"");runtime.set_rate(48000);std::vector<int16_t> original(48000*2);
 for(int i=0;i<48000;++i)original[i*2]=original[i*2+1]=int16_t(std::sin(6.283185307179586*440*i/48000)*12000);
 runtime.remember(original.data(),48000);int16_t out[256*2]{};int64_t frames=0;int16_t previous=original.back();int largest_jump=0;double early=0,late=0;
 for(;;){int n=runtime.render_bridge(out,256);if(!n)break;for(int i=0;i<n;++i){largest_jump=std::max(largest_jump,std::abs(int(out[i*2])-previous));previous=out[i*2];if(frames<4800)early+=double(out[i*2])*out[i*2];if(frames>int64_t(48000*(s.adaptive.bridge_max_ms-s.adaptive.bridge_fade_ms)/1000))late+=double(out[i*2])*out[i*2];++frames;}}
 runtime.speed_start=.94;runtime.last_speed=1.06;runtime.input_fraction=.9;runtime.warm=17;runtime.reset_seek(44100);
 assert(runtime.rate==44100 && runtime.speed_start==1 && runtime.last_speed==1 && runtime.input_fraction==0 && runtime.warm==0 && !runtime.band_active && runtime.history_count==0);
 assert(frames==int64_t(48000*s.adaptive.bridge_max_ms/1000));assert(largest_jump<5000);assert(early>0 && late>0);assert(std::abs(int(previous))<20);
 std::printf("emergency bridge actual runtime: frames=%lld maximum sample jump=%d end=%d\n",(long long)frames,largest_jump,previous);
}
void test_stem_runtime(const AudioTransitionSettings &s){
 constexpr int rate=48000;
 for(int scenario=0;scenario<7;++scenario){
  double duration=scenario==4?s.full.vocal_fade_ms:s.manual_ms;
  int frames=int(rate*duration/1000)+512;EngineTransition runtime(s,"");runtime.set_rate(rate);
  auto job=std::make_shared<EngineTransition::Request>();job->source=std::make_unique<SystemDecode>();job->source->output_end=true;job->source->first_media_us=0;job->source->prepared.resize(frames*2);job->rate=rate;job->blend_ms=duration;job->plan.duration_ms=duration;job->plan.gain_b=1;job->plan.mode=lyra::adaptive::Mode::TextureBridge;
  auto old=std::make_unique<SystemDecode>();old->output_end=true;old->first_media_us=0;old->prepared.resize(frames*2);
  for(auto *map:{&job->full_a,&job->full_b}){map->header.stems=3;map->header.hop_ms=s.full.hop_ms;map->rows.resize(64);for(auto &row:map->rows)row.voice=.8f;}
  if(scenario!=0)for(auto &row:job->full_a.rows)row.voice=0;
  for(auto *clip:{&job->stem_a,&job->stem_b}){clip->rate=rate;clip->start_us=0;clip->pcm.resize(frames*2);}
  for(int i=0;i<frames;++i){int16_t a=int16_t(6000*std::cos(6.283185307179586*220*i/rate)),b=int16_t(6000*std::cos(6.283185307179586*(scenario==5?90:scenario==6?3000:440)*i/rate));old->prepared[i*2]=a;job->source->prepared[i*2+1]=b;job->stem_a.pcm[i*2]=a;job->stem_b.pcm[i*2+1]=b;}
  if(scenario==5){std::fill(job->stem_a.pcm.begin(),job->stem_a.pcm.end(),0);std::fill(job->stem_b.pcm.begin(),job->stem_b.pcm.end(),0);}
  if(scenario==6){job->stem_a.pcm.clear();job->stem_b.pcm.clear();}
  if(scenario==2)job->stem_a.pcm.clear();
  if(scenario==3)job->stem_a.pcm.resize(2);
  runtime.begin(job,std::move(old));bool separated=scenario!=2 && scenario!=3 && scenario!=6;assert(runtime.stem_active==separated && !runtime.stretch && runtime.speed_start==1);
  std::atomic<bool> cancel{false};int16_t block[512]{};double speed=0;int64_t count=0;double outgoing_early=0,incoming_late=0,incoming_early=0,reference_early=0;int first=-1,last=0;
  while(runtime.outgoing){int n=runtime.render(*job->source,block,256,cancel,&speed);assert(n>0 && speed==1);runtime.mix(block,n,cancel);
   for(int i=0;i<n;++i){if(!count)first=block[i*2];last=block[i*2+1];if(count<rate*duration/4000)outgoing_early+=double(block[i*2])*block[i*2];if(count>rate*duration*3/4000)incoming_late+=double(block[i*2+1])*block[i*2+1];if(count>=rate*duration/8000 && count<rate*duration/4000){incoming_early+=double(block[i*2+1])*block[i*2+1];double ref=job->source->prepared[size_t(count)*2+1];reference_early+=ref*ref;}++count;}}
  // Quiet-map classification and missing/truncated caches must never erase the
  // first audible outgoing sample. Both endpoints must finish within the mix.
  if(scenario!=4){std::fprintf(stderr,"incoming runtime scenario=%d early RMS ratio=%g\n",scenario,std::sqrt(incoming_early/reference_early));assert(incoming_early>reference_early*.04);}
  assert(first>5000 && outgoing_early>0 && incoming_late>0 && !runtime.mixing);
  assert(std::abs(last-int(job->source->prepared[size_t(count-1)*2+1]))<=2);
  if(separated){assert(runtime.vocals.exit_s>=runtime.vocals.fade_s);assert(runtime.vocals.exit_s+runtime.vocals.fade_s<=double(runtime.mix_frames)/rate+1./rate);}
  runtime.reset_seek(rate);assert(!runtime.stem_active && runtime.stem_a.pcm.empty() && runtime.stem_b.pcm.empty() && runtime.last_speed==1);
  std::printf("stem boundary scenario=%d separated=%d first_outgoing=%d frames=%lld passed\n",scenario,separated,first,(long long)count);
 }
}

void test_tagged_flac(const char *path){
 int input=open(path,O_RDONLY);struct stat st{};assert(input>=0 && !fstat(input,&st) && st.st_size>4096 && st.st_size<1048576);
 std::vector<uint8_t> file(size_t(st.st_size));assert(pread(input,file.data(),file.size(),0)==ssize_t(file.size()));close(input);assert(!memcmp(file.data(),"fLaC",4));
 std::vector<uint8_t> tag(8192,0);memcpy(tag.data(),"ID3",3);tag[3]=4;size_t length=tag.size()-10;for(int i=9;i>=6;--i){tag[size_t(i)]=uint8_t(length&127);length>>=7;}
 size_t at=file.size()/2;file.insert(file.begin()+at,tag.begin(),tag.end());char name[]="/data/local/tmp/lyra-tagged-XXXXXX";int fd=mkstemp(name);assert(fd>=0);unlink(name);assert(write(fd,file.data(),file.size())==ssize_t(file.size()));
 SystemDecode decoder;decoder.fd=fd;assert(decoder.open());std::atomic<bool> cancel{false};int16_t block[512]{};int frames=0;
 for(;;){int n=decoder.pull(block,256,48000,cancel);assert(n>=0);if(!n)break;frames+=n;}
 assert(frames>48000*3.5 && frames<=48000*4);std::printf("midstream ID3: recover past invalid packet to EOF, frames=%d passed\n",frames);
}
