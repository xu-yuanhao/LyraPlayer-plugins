#pragma once
#include "worker_wire.h"
#include "plugin/stem_contracts.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <cmath>
extern char **environ;
namespace lyra_stems {
// Native worker only: no Java runtime, player, audio output or second playback state.
class ProcessModel {
 int socket_=-1;pid_t child_=-1;std::string executable_;std::vector<float> output_;
public:
 ~ProcessModel(){release();}
 void executable(std::string path){executable_=std::move(path);}
 void release(){if(socket_>=0){close(socket_);socket_=-1;}if(child_>0){kill(child_,SIGTERM);int status;while(waitpid(child_,&status,0)<0 && errno==EINTR){}child_=-1;}output_.clear();output_.shrink_to_fit();}
 bool load(const std::string &param,const std::string &weights,int){
  if(child_>0)return true;int sockets[2];if(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets))return false;
  std::string parent=std::to_string(getpid());char linker[]="/system/bin/linker64";char *args[]={linker,const_cast<char*>(executable_.c_str()),const_cast<char*>(param.c_str()),const_cast<char*>(weights.c_str()),const_cast<char*>(parent.c_str()),nullptr};
  long limit=sysconf(_SC_OPEN_MAX);if(limit<0)limit=1024;
  // Only async-signal-safe descriptor operations and exec in the fork child.
  // The network and its allocator are constructed after exec, never in a forked ART heap.
  child_=fork();if(child_==0){if(dup2(sockets[1],0)<0 || dup2(sockets[1],1)<0)_exit(126);for(int fd=3;fd<limit;++fd)close(fd);execve(linker,args,environ);_exit(127);}
  close(sockets[1]);socket_=sockets[0];if(child_<0){release();return false;}
  uint32_t hello=0;if(!transfer(socket_,&hello,sizeof(hello),false) || hello!=wire_magic){release();return false;}output_.resize(size_t(LYRA_STEM_INPUT_FRAMES)*2);return true;
 }
 bool process(const float *pcm,size_t samples){if(child_<=0 || samples!=size_t(LYRA_STEM_INPUT_FRAMES)*2)return false;for(size_t i=0;i<samples;++i)if(!std::isfinite(pcm[i]) || std::abs(pcm[i])>1.001f)return false;uint32_t count=uint32_t(samples*sizeof(float)),returned=0;bool ok=transfer(socket_,&count,sizeof(count),true) && transfer(socket_,const_cast<float*>(pcm),count,true) && transfer(socket_,&returned,sizeof(returned),false) && returned==count && transfer(socket_,output_.data(),count,false);if(!ok)release();return ok;}
 const std::vector<float> &output()const{return output_;}
};
}
