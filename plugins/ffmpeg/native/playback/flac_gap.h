#pragma once
#include "container_probe.h"
#include <algorithm>
#include <vector>
namespace lyra::audio {
struct FlacGap { int64_t at=0,bytes=0; };
inline unsigned flac_crc(unsigned crc,unsigned byte,unsigned polynomial,unsigned mask){
    crc^=byte<<(mask==255?0:8);
    for(int i=0;i<8;++i)crc=((crc<<1)^((crc&(mask==255?128:32768))?polynomial:0))&mask;
    return crc;
}
// Validate a first-frame header and return its byte length. Match STREAMINFO,
// require canonical frame/sample number zero, and verify FLAC's header CRC8.
inline size_t flac_first_header(const uint8_t *p,size_t n,int channels,int bps,bool first=true){
    if(n<16 || p[0]!=255 || (p[1]&254)!=248 || (p[2]>>4)==0 || (p[2]&15)==15 ||
       (p[3]&1) || (p[3]>>4)>10 || (first && p[4]!=0))return 0;
    int ch=p[3]>>4;if((ch<=7?ch+1:2)!=channels)return 0;
    static constexpr int depths[]={0,8,12,-1,16,20,24,32};
    int depth=depths[(p[3]>>1)&7];if(depth<0 || (depth && depth!=bps))return 0;
    size_t len=5;
    if(p[4]&128){
        int leading=0;for(unsigned bit=128;p[4]&bit;bit>>=1)++leading;
        if(leading<2 || leading>7 || size_t(leading+4)>=n)return 0;
        for(int i=1;i<leading;++i)if((p[4+i]&192)!=128)return 0;
        len=4+size_t(leading);
    }
    int block=p[2]>>4,rate=p[2]&15;
    if(block==6)++len;else if(block==7)len+=2;
    if(rate==12)++len;else if(rate==13 || rate==14)len+=2;
    unsigned crc=0;for(size_t i=0;i<=len;++i)crc=flac_crc(crc,p[i],7,255);
    return crc?0:len+1;
}
// Recover only a CRC-verified complete first frame, never a bare sync marker.
// Bound work and memory to a header-sized scan window, preserve the original fd.
inline FlacGap flac_leading_gap(int fd,int64_t size){
    int64_t start=flac_payload_offset(fd,size);if(start<0)return {};
    uint8_t info[34]{},h[4]{};int64_t at=start+4;bool found=false;
    for(;;){
        if(at>size-4 || pread(fd,h,4,at)!=4)return {};
        int64_t n=(int64_t(h[1])<<16)|(int64_t(h[2])<<8)|h[3];
        if(n>size-at-4)return {};
        if((h[0]&127)==0){if(n!=34 || pread(fd,info,34,at+4)!=34)return {};found=true;}
        at+=4+n;if(h[0]&128)break;
    }
    if(!found)return {};
    int channels=((info[12]>>1)&7)+1,bps=((info[12]&1)<<4)|(info[13]>>4);++bps;
    size_t max_frame=(size_t(info[7])<<16)|(size_t(info[8])<<8)|info[9];
    if(!max_frame)max_frame=1024*1024;
    uint8_t first[16]{};
    if(pread(fd,first,sizeof(first),at)!=sizeof(first) || flac_first_header(first,sizeof(first),channels,bps))return {};
    constexpr size_t scan_budget=32*1024*1024,chunk=64*1024;
    std::vector<uint8_t> buffer(chunk+16),frame;
    int64_t end=std::min(size-int64_t(sizeof(first)),at+int64_t(scan_budget));
    for(int64_t pos=at;pos<end;pos+=chunk){
        size_t n=size_t(std::min(int64_t(buffer.size()),size-pos));
        if(pread(fd,buffer.data(),n,pos)!=ssize_t(n))return {};
        for(size_t i=0;i<std::min(chunk,n-sizeof(first));++i){
            size_t header=flac_first_header(buffer.data()+i,n-i,channels,bps);if(!header)continue;
            size_t count=size_t(std::min(int64_t(max_frame+16),size-pos-int64_t(i)));
            frame.resize(count);if(pread(fd,frame.data(),count,pos+int64_t(i))!=ssize_t(count))return {};
            unsigned crc=0;
            for(size_t j=0;j<count;++j){
                crc=flac_crc(crc,frame[j],0x8005,65535);
                // A frame CRC16 closes immediately before the next sync (or EOF).
                if(j>header && !crc && (pos+int64_t(i+j+1)==size ||
                   (j+16<count && flac_first_header(frame.data()+j+1,count-j-1,channels,bps,false))))
                    return {at,pos+int64_t(i)-at};
            }
        }
    }
    return {};
}
}
