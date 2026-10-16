#pragma once
#include <cstdint>
#include <cstring>
#include <unistd.h>
namespace lyra::audio {
// Borrow the fd; pread preserves the decoder and metadata readers' positions.
// Inspect only the fixed headers, even when an ID3 artwork tag is very large.
inline int64_t flac_payload_offset(int fd,int64_t length){
 uint8_t header[10]{};if(length<int64_t(sizeof(header)) || pread(fd,header,sizeof(header),0)!=sizeof(header))return -1;
 int64_t offset=0;
 if(!std::memcmp(header,"ID3",3)){
  if(header[3]<2 || header[3]>4 || ((header[6]|header[7]|header[8]|header[9])&128))return -1;
  int64_t size=0;for(int i=6;i<10;++i)size=(size<<7)|header[i];
  offset=int64_t(sizeof(header))+size+((header[3]==4 && (header[5]&16))?int64_t(sizeof(header)):0);
 }
 char magic[4]{};if(offset>length-int64_t(sizeof(magic)) || pread(fd,magic,sizeof(magic),offset)!=sizeof(magic))return -1;
 return !std::memcmp(magic,"fLaC",sizeof(magic))?offset:-1;
}
}
