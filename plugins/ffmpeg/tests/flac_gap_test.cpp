#include "playback/flac_gap.h"
#include <cassert>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <cstdio>
int main(int argc,char **argv){
 assert(argc==2);std::ifstream in(argv[1],std::ios::binary);
 std::vector<uint8_t> source((std::istreambuf_iterator<char>(in)),{});size_t at=4;
 for(;;){auto tag=source[at];size_t n=(size_t(source[at+1])<<16)|(size_t(source[at+2])<<8)|source[at+3];at+=4+n;if(tag&128)break;}
 for(size_t gap:{size_t(0),size_t(1),size_t(65535),size_t(65536),size_t(65537),size_t(16777216)}){
  auto data=source;data.insert(data.begin()+at,gap,97);
  // A plausible header with an invalid frame must never win over the real one.
  if(gap>65536)std::copy_n(source.data()+at,16,data.data()+at+31);
  char path[]="/tmp/lyra-flac-gap-XXXXXX";int fd=mkstemp(path);assert(fd>=0);unlink(path);
  assert(write(fd,data.data(),data.size())==ssize_t(data.size()));
  auto found=lyra::audio::flac_leading_gap(fd,data.size());
  assert(found.bytes==int64_t(gap));if(gap)assert(found.at==int64_t(at));close(fd);
 }
 std::puts("FLAC gap CRC validation, decoys, read boundaries and 16MiB recovery passed");
}
