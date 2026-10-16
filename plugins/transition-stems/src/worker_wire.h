#pragma once
#include <sys/socket.h>
#include <poll.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
namespace lyra_stems {
constexpr uint32_t wire_magic=0x53544d31;
inline bool transfer(int fd,void *bytes,size_t count,bool sending,int timeout=5000){auto *p=static_cast<char*>(bytes);while(count){pollfd wait{fd,short(sending?POLLOUT:POLLIN),0};int ready;do{ready=poll(&wait,1,timeout);}while(ready<0 && errno==EINTR);if(ready<=0 || (wait.revents&(POLLERR|POLLNVAL)))return false;ssize_t n=sending?send(fd,p,count,MSG_NOSIGNAL):recv(fd,p,count,0);if(n<0 && errno==EINTR)continue;if(n<=0)return false;p+=n;count-=size_t(n);}return true;}
}
