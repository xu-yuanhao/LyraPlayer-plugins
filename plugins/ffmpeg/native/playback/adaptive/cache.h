#pragma once
#include "playback/adaptive/analysis.h"
#include <cstdio>
#include <string>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
namespace lyra::adaptive {
inline uint64_t map_hash(const void *bytes,size_t count,uint64_t seed=1469598103934665603ULL){auto *p=static_cast<const unsigned char*>(bytes);for(size_t i=0;i<count;++i)seed=(seed^p[i])*1099511628211ULL;return seed;}
inline uint64_t map_key(int fd,const int16_t *pcm,size_t frames,int rate,const AudioTransitionSettings &s){
 struct stat st{};if(fstat(fd,&st))return 0;uint64_t key=map_hash(&st.st_size,sizeof(st.st_size));
 key=map_hash(&st.st_mtim,sizeof(st.st_mtim),key);key=map_hash(&st.st_ino,sizeof(st.st_ino),key);key=map_hash(&st.st_dev,sizeof(st.st_dev),key);key=map_hash(&rate,sizeof(rate),key);
 const double parameters[]={s.analysis_ms,s.feature_hz,s.bass_hz,s.bpm_min,s.bpm_max,s.onset_floor,s.peak_factor,s.minimum_onsets,s.adaptive.split_hz,s.adaptive.smooth_ms,s.adaptive.tolerance_ms};
 key=map_hash(parameters,sizeof(parameters),key);key=map_hash(&frames,sizeof(frames),key);
 for(size_t i=0;i<frames;i+=128)key=map_hash(pcm+i*2,sizeof(int16_t)*2,key);
 return key;
}
struct CacheRecord {uint64_t magic=0,key=0;TrackMap map;uint64_t checksum=0;};
inline bool valid_map(const TrackMap &m){
 if(m.version!=1 || m.beats>m.grid.size() || !std::isfinite(m.bpm) || m.bpm<0 || m.bpm>300 || !std::isfinite(m.confidence) || m.confidence<0 || m.confidence>1)return false;
 const double values[]={m.first_s,m.last_s,m.span_s,m.rms,m.bass,m.mid,m.air,m.attack};for(double x:values)if(!std::isfinite(x) || x<0)return false;
 if(m.first_s>m.last_s || m.last_s>m.span_s || m.rms>1 || m.bass>1 || m.mid>1 || m.air>1)return false;
 double last=0;for(uint32_t i=0;i<m.beats;++i){if(!std::isfinite(m.grid[i]) || m.grid[i]<last || m.grid[i]>m.span_s)return false;last=m.grid[i];}return true;
}
inline std::string map_path(const std::string &root,uint64_t key){char name[64];std::snprintf(name,sizeof(name),"/map-%016llx.bin",(unsigned long long)key);return root+name;}
inline bool cache_read(const std::string &root,uint64_t key,TrackMap *out){
 if(root.empty() || !key)return false;auto path=map_path(root,key);FILE *file=std::fopen(path.c_str(),"rb");if(!file)return false;
 CacheRecord value{};bool ok=std::fread(&value,1,sizeof(value),file)==sizeof(value) && std::fgetc(file)==EOF;std::fclose(file);
 ok=ok && value.magic==0x4c5952414d415031ULL && value.key==key && value.checksum==map_hash(&value.map,sizeof(value.map)) && valid_map(value.map);
 if(ok)*out=value.map;return ok;
}
inline void cache_write(const std::string &root,uint64_t key,const TrackMap &map,int maximum){
 if(root.empty() || !key || !valid_map(map))return;mkdir(root.c_str(),0700);
 auto path=map_path(root,key),temp=path+".tmp";CacheRecord value{};value.magic=0x4c5952414d415031ULL;value.key=key;value.map=map;value.checksum=map_hash(&value.map,sizeof(value.map));
 FILE *file=std::fopen(temp.c_str(),"wb");if(!file)return;bool ok=std::fwrite(&value,1,sizeof(value),file)==sizeof(value);ok=std::fclose(file)==0 && ok;if(!ok || rename(temp.c_str(),path.c_str())){unlink(temp.c_str());return;}
 DIR *dir=opendir(root.c_str());if(!dir)return;int count=0;std::string oldest;time_t earliest=0;
 while(auto *entry=readdir(dir)){std::string name=entry->d_name;if(name.size()!=24 || name.compare(0,4,"map-") || name.compare(20,4,".bin"))continue;++count;std::string p=root+"/"+name;struct stat st{};if(!stat(p.c_str(),&st) && (oldest.empty() || st.st_mtime<earliest)){oldest=p;earliest=st.st_mtime;}}
 closedir(dir);if(count>maximum && !oldest.empty())unlink(oldest.c_str());
}
}
