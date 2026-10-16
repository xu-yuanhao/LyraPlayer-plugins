#pragma once
#include "playback/adaptive/cache.h"
#include "plugin/stem_contracts.h"
#include "third_party/signalsmith/linear/fft.h"
#include <complex>
#include <array>
#include <fcntl.h>
namespace lyra::adaptive {
inline uint64_t full_signature(const FullTrackSettings &s){const double v[]={s.hop_ms,s.novelty_ms,s.novelty_threshold,s.phrase_min_ms,s.vocal_threshold,s.vocal_floor_db,s.vocal_smooth_ms,s.vocal_guard_ms,s.chroma_low_hz,s.chroma_high_hz};return map_hash(v,sizeof(v));}
struct FullRow {
 float energy=0,bass=0,air=0,chroma[12]{},voice=-1,vocal_rms=0,novelty=0,bpm=0,beat_confidence=0;
};
struct Phrase {double seconds=0,confidence=0;};
struct FullHeader {
 uint64_t magic=0x4c59524146554c31ULL,key=0,checksum=0,configuration=0;
 uint32_t version=1,rate=LYRA_STEM_RATE,rows=0,stems=0;
 int64_t start_us=0,frames=0,duration_us=0;
 double hop_ms=0,bpm=0,confidence=0;
};
struct FullMap {
 FullHeader header;std::vector<FullRow> rows;std::vector<Phrase> phrases;
 const FullRow *at(double seconds)const{double relative=seconds-double(header.start_us)/1000000;if(relative<0 || rows.empty() || header.hop_ms<=0)return nullptr;size_t i=size_t(relative*1000/header.hop_ms);return i<rows.size()?&rows[i]:nullptr;}
 double next_voice_gap(double seconds,double window,const FullTrackSettings &s)const{
  if(!header.stems)return -1;auto *first=at(seconds);if(!first)return -1;if(first->voice<s.vocal_threshold)return 0;
  int guard=std::max(1,int(std::ceil(s.vocal_smooth_ms/header.hop_ms))),quiet=0;
  for(double t=seconds;t<=seconds+window;t+=header.hop_ms/1000){auto *row=at(t);if(!row)break;quiet=row->voice<s.vocal_threshold?quiet+1:0;if(quiet>=guard)return std::max(0.,t-seconds-(quiet-1)*header.hop_ms/1000-s.vocal_guard_ms/1000);}
  return -1;
 }
 double first_voice(double seconds,double window,const FullTrackSettings &s)const{
  if(!header.stems)return -1;for(double t=seconds;t<=seconds+window;t+=header.hop_ms/1000){auto *row=at(t);if(row && row->voice>=s.vocal_threshold)return t-seconds;}return window;
 }
};
inline uint64_t full_key(int fd){
 struct stat st{};if(fd<0 || fstat(fd,&st))return 0;uint64_t key=map_hash(&st.st_size,sizeof(st.st_size));key=map_hash(&st.st_mtim,sizeof(st.st_mtim),key);key=map_hash(&st.st_ino,sizeof(st.st_ino),key);return map_hash(&st.st_dev,sizeof(st.st_dev),key);
}
inline std::string full_path(const std::string &root,uint64_t key,const char *suffix){char name[64];std::snprintf(name,sizeof(name),"/full-%016llx.%s",(unsigned long long)key,suffix);return root+name;}
inline bool full_valid(const FullMap &map,const FullTrackSettings &s){
 const auto &h=map.header;if(h.magic!=0x4c59524146554c31ULL || h.version!=1 || h.rate!=LYRA_STEM_RATE || (h.stems>3 || h.stems==2) || h.configuration!=full_signature(s) || h.frames<=0 || h.frames>int64_t(LYRA_STEM_RATE*s.max_ms/1000) || h.start_us<0 || h.start_us>s.max_ms*1000 || h.duration_us<0 || h.duration_us>s.max_ms*1000 || !std::isfinite(h.hop_ms) || h.hop_ms!=s.hop_ms || !std::isfinite(h.bpm) || h.bpm<0 || h.bpm>300 || !std::isfinite(h.confidence) || h.confidence<0 || h.confidence>1 || h.rows!=map.rows.size() || h.rows>size_t(std::ceil(s.max_ms/s.hop_ms)))return false;
 if(h.rows!=size_t(std::ceil(double(h.frames)*1000/(h.rate*h.hop_ms))))return false;
 for(const auto &r:map.rows){const float values[]={r.energy,r.bass,r.air,r.voice,r.vocal_rms,r.novelty,r.bpm,r.beat_confidence};for(float x:values)if(!std::isfinite(x))return false;if(r.energy<0 || r.energy>1 || r.bass<0 || r.bass>1 || r.air<0 || r.air>1 || r.voice< -1 || r.voice>1 || r.vocal_rms<0 || r.vocal_rms>2 || r.novelty<0 || r.novelty>1 || r.bpm<0 || r.bpm>300 || r.beat_confidence<0 || r.beat_confidence>1)return false;for(float x:r.chroma)if(!std::isfinite(x) || x<0 || x>1)return false;}
 return true;
}
inline void find_phrases(FullMap &map,const FullTrackSettings &s){
 map.phrases.clear();if(map.rows.empty())return;double origin=double(map.header.start_us)/1000000;std::vector<Phrase> candidates;
 int radius=std::max(1,int(s.novelty_ms/map.header.hop_ms));
 for(size_t i=1;i+1<map.rows.size();++i){const auto &r=map.rows[i];
  if(r.novelty<s.novelty_threshold || r.novelty<map.rows[i-1].novelty || r.novelty<=map.rows[i+1].novelty)continue;
  bool best=true;for(size_t j=i+1;j<std::min(map.rows.size(),i+size_t(radius/2));++j)if(map.rows[j].novelty>r.novelty){best=false;break;}
  if(best)candidates.push_back({origin+i*map.header.hop_ms/1000,r.novelty});
 }
 if(map.header.stems&1){size_t gap=0;float preceding=0;
  for(size_t i=0;i<map.rows.size();++i){float voice=map.rows[i].voice;if(voice>=s.vocal_threshold){
    if(gap && (i-gap)*map.header.hop_ms>=s.vocal_smooth_ms){double confidence=std::sqrt(std::max(0.f,preceding)*voice);candidates.push_back({origin+(gap*map.header.hop_ms+s.vocal_guard_ms)/1000,std::clamp(confidence,0.,1.)});}
    gap=0;preceding=voice;
   }else if(!gap && i>0 && map.rows[i-1].voice>=s.vocal_threshold)gap=i;
  }
  if(gap && (map.rows.size()-gap)*map.header.hop_ms>=s.vocal_smooth_ms)candidates.push_back({origin+(gap*map.header.hop_ms+s.vocal_guard_ms)/1000,preceding});
 }
 std::sort(candidates.begin(),candidates.end(),[](const Phrase &a,const Phrase &b){return a.confidence>b.confidence;});map.phrases.push_back({origin,1});
 for(const auto &candidate:candidates){bool distinct=true;for(const auto &kept:map.phrases)if(std::abs(candidate.seconds-kept.seconds)<s.phrase_min_ms/1000){distinct=false;break;}if(distinct)map.phrases.push_back(candidate);}
 std::sort(map.phrases.begin(),map.phrases.end(),[](const Phrase &a,const Phrase &b){return a.seconds<b.seconds;});
}
inline bool read_full_map(const std::string &root,uint64_t key,const FullTrackSettings &s,FullMap *out){
 if(root.empty() || !key || !s.enabled)return false;auto path=full_path(root,key,"index");FILE *f=std::fopen(path.c_str(),"rb");if(!f)return false;
 FullMap result;bool ok=std::fread(&result.header,1,sizeof(result.header),f)==sizeof(result.header);auto &h=result.header;
 ok=ok && h.key==key && h.rows>0 && h.rows<=size_t(std::ceil(s.max_ms/s.hop_ms));if(ok){result.rows.resize(h.rows);ok=std::fread(result.rows.data(),sizeof(FullRow),h.rows,f)==h.rows && std::fgetc(f)==EOF;}std::fclose(f);
 if(!ok || !full_valid(result,s) || h.checksum!=map_hash(result.rows.data(),result.rows.size()*sizeof(FullRow)))return false;
 if(h.stems&2){struct stat st{};auto vocals=full_path(root,key,"vocals");if(stat(vocals.c_str(),&st) || st.st_size!=h.frames*int64_t(sizeof(int16_t))*2)return false;}
 find_phrases(result,s);*out=std::move(result);return true;
}
inline bool write_full_map(const std::string &root,FullMap &map,const FullTrackSettings &s){
 map.header.rows=uint32_t(map.rows.size());if(!full_valid(map,s))return false;map.header.checksum=map_hash(map.rows.data(),map.rows.size()*sizeof(FullRow));
 auto path=full_path(root,map.header.key,"index"),temp=path+".tmp";FILE *f=std::fopen(temp.c_str(),"wb");if(!f)return false;bool ok=std::fwrite(&map.header,1,sizeof(map.header),f)==sizeof(map.header) && std::fwrite(map.rows.data(),sizeof(FullRow),map.rows.size(),f)==map.rows.size();ok=std::fclose(f)==0 && ok;if(!ok || rename(temp.c_str(),path.c_str())){unlink(temp.c_str());return false;}return true;
}
class FullFeatures {
 signalsmith::linear::RealFFT<float> fft_{LYRA_STEM_FFT};std::vector<float> pcm_,window_,time_;std::vector<std::complex<float>> spec_;FullMap *map_;FullTrackSettings settings_;AudioTransitionSettings audio_;int hop_=1,used_=0;double energy_=0,vocal_=0;
 void row(){if(!used_)return;FullRow r;r.energy=float(std::sqrt(energy_/used_));r.vocal_rms=float(std::sqrt(vocal_/used_));r.voice=map_->header.stems?std::clamp(float(vocal_/(energy_+std::numeric_limits<double>::epsilon())),0.f,1.f):-1;
  if(r.vocal_rms<std::pow(10.,settings_.vocal_floor_db/20))r.voice=map_->header.stems?0:-1;
  size_t frames=pcm_.size()/2;double total=0,lo=0,hi=0;for(int c=0;c<2;++c){for(int i=0;i<LYRA_STEM_FFT;++i){size_t back=size_t(LYRA_STEM_FFT-i);time_[i]=back<=frames?pcm_[(frames-back)*2+c]*window_[i]:0;}fft_.fft(time_.data(),spec_.data());
   for(int b=1;b<LYRA_STEM_FFT/2;++b){double e=std::norm(spec_[b]),hz=double(b)*LYRA_STEM_RATE/LYRA_STEM_FFT;total+=e;if(hz<audio_.bass_hz)lo+=e;if(hz>audio_.adaptive.split_hz)hi+=e;if(hz>=settings_.chroma_low_hz && hz<=settings_.chroma_high_hz){int note=int(std::lround(69+12*std::log2(hz/440)));r.chroma[(note%12+12)%12]+=float(std::sqrt(e));}}
  }
  r.bass=total>0?float(lo/total):0;r.air=total>0?float(hi/total):0;float chroma=0;for(float x:r.chroma)chroma+=x;for(float &x:r.chroma)x=chroma>0?x/chroma:0;
  map_->rows.push_back(r);used_=0;energy_=vocal_=0;
 }
public:
 FullFeatures(FullMap &map,const AudioTransitionSettings &audio):map_(&map),settings_(audio.full),audio_(audio){const auto &s=audio.full;hop_=std::max(1,int(LYRA_STEM_RATE*s.hop_ms/1000));time_.resize(LYRA_STEM_FFT);window_.resize(LYRA_STEM_FFT);spec_.resize(LYRA_STEM_FFT/2);pcm_.reserve(LYRA_STEM_FFT*2);for(int i=0;i<LYRA_STEM_FFT;++i)window_[i]=float(.5-.5*std::cos(6.283185307179586*i/LYRA_STEM_FFT));}
 void feed(const int16_t *pcm,const float *vocals,int frames){
  for(int i=0;i<frames;++i){float a=pcm[i*2]/32768.f,b=pcm[i*2+1]/32768.f;energy_+=(a*a+b*b)*.5;if(vocals)vocal_+=(double(vocals[i*2])*vocals[i*2]+double(vocals[i*2+1])*vocals[i*2+1])*.5;
   pcm_.push_back(a);pcm_.push_back(b);if(pcm_.size()>size_t(LYRA_STEM_FFT*4))pcm_.erase(pcm_.begin(),pcm_.end()-LYRA_STEM_FFT*2);if(++used_==hop_)row();
  }
 }
 void finish(){row();int radius=std::max(1,int(settings_.novelty_ms/settings_.hop_ms));
  for(size_t i=0;i<map_->rows.size();++i){std::array<double,15> before{},after{};int a=0,b=0;
   auto add=[](std::array<double,15> &v,const FullRow &r){for(int j=0;j<12;++j)v[j]+=r.chroma[j];v[12]+=std::sqrt(r.energy);v[13]+=r.bass;v[14]+=r.air;};
   for(size_t j=i>size_t(radius)?i-radius:0;j<i;++j){add(before,map_->rows[j]);++a;}for(size_t j=i;j<std::min(map_->rows.size(),i+size_t(radius));++j){add(after,map_->rows[j]);++b;}
   double difference=0;if(a && b){for(size_t j=0;j<before.size();++j)difference+=std::abs(before[j]/a-after[j]/b);map_->rows[i].novelty=float(std::clamp(difference/std::sqrt(double(before.size())),0.,1.));}
  }
  find_phrases(*map_,settings_);
 }
};
struct StemClip {
 int rate=0;int64_t start_us=0;std::vector<int16_t> pcm;
 bool contains(int64_t position_us,int64_t span_us)const{return rate>0 && position_us>=start_us && position_us+span_us<=start_us+int64_t(pcm.size()/2)*1000000/rate;}
 void sample(int64_t frame,float out[2])const{if(frame<0 || size_t(frame)>=pcm.size()/2){out[0]=out[1]=0;return;}out[0]=pcm[size_t(frame)*2]/32768.f;out[1]=pcm[size_t(frame)*2+1]/32768.f;}
};
inline bool read_stem_clip(const std::string &root,const FullMap &map,int64_t position_us,int output_rate,double window_ms,StemClip *out){
 const auto &h=map.header;if(!(h.stems&2) || position_us<h.start_us || output_rate<8000)return false;
 int64_t first=(position_us-h.start_us)*h.rate/1000000,count=std::min<int64_t>(h.frames-first,int64_t(h.rate*window_ms/1000)+1);if(first<0 || count<2)return false;
 auto path=full_path(root,h.key,"vocals");FILE *f=std::fopen(path.c_str(),"rb");if(!f)return false;std::vector<int16_t> source(size_t(count)*2);bool ok=std::fseek(f,long(first*2*sizeof(int16_t)),SEEK_SET)==0 && std::fread(source.data(),sizeof(int16_t)*2,size_t(count),f)==size_t(count);std::fclose(f);if(!ok)return false;
 StemClip result;result.rate=output_rate;result.start_us=h.start_us+first*1000000/h.rate;size_t frames=size_t((count-1)*output_rate/h.rate);result.pcm.resize(frames*2);
 for(size_t i=0;i<frames;++i){double at=double(i)*h.rate/output_rate;size_t a=size_t(at),b=std::min(a+1,size_t(count-1));float fraction=float(at-a);for(int c=0;c<2;++c)result.pcm[i*2+c]=int16_t(source[a*2+c]+fraction*(source[b*2+c]-source[a*2+c]));}
 *out=std::move(result);return true;
}
struct VocalHandoff {bool enabled=false;double wait_s=0,exit_s=0,fade_s=0;};
// Two consecutive vocal ramps must fit completely inside the handover.
inline void fit_vocal_handoff(VocalHandoff &p,double budget){
 if(budget<=0 || p.fade_s<=0){p.enabled=false;return;}
 p.fade_s=std::min(p.fade_s,budget/2);p.exit_s=std::clamp(p.exit_s,p.fade_s,budget-p.fade_s);
}
inline VocalHandoff vocal_handoff(const FullMap &a,const FullMap &b,double a_seconds,double b_seconds,double budget,const FullTrackSettings &s){
 VocalHandoff p;if(!a.header.stems || !b.header.stems)return p;p.enabled=true;p.fade_s=s.vocal_fade_ms/1000;
 double gap=a.next_voice_gap(a_seconds,budget+s.wait_ms/1000,s),voice=b.first_voice(b_seconds,std::min(budget,s.intro_ms/1000),s);
 if(gap<0)p.exit_s=voice>0?std::min(voice,budget):p.fade_s;
 else{p.wait_s=std::clamp(gap-voice,0.,s.wait_ms/1000);p.exit_s=std::clamp(gap-p.wait_s,0.,std::max(0.,voice));}
 fit_vocal_handoff(p,budget);return p;
}
}
