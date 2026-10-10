#include "platform/audio_output.h"
#include "platform/log.h"
#include <aaudio/AAudio.h>
#include <dlfcn.h>
#include <algorithm>
#include <time.h>
namespace lyra::audio {
static_assert(int(Closed)==AAUDIO_STREAM_STATE_CLOSED&&int(Disconnected)==AAUDIO_STREAM_STATE_DISCONNECTED);
static_assert(ErrorTimeout==AAUDIO_ERROR_TIMEOUT&&ErrorInternal==AAUDIO_ERROR_INTERNAL&&ErrorInvalidState==AAUDIO_ERROR_INVALID_STATE&&ErrorDisconnected==AAUDIO_ERROR_DISCONNECTED);
struct Stream {AAudioStream* handle=nullptr;ErrorCallback error=nullptr;void* user=nullptr;};
static void failed(AAudioStream*,void* user,aaudio_result_t error){auto* s=static_cast<Stream*>(user);if(s->error)s->error(s->user,error);}
Stream* open(int preferred,const AudioOutputSettings& settings,ErrorCallback callback,void* user) {
    int rates[2],count=0;if(preferred>=8000&&preferred<=384000)rates[count++]=preferred;if(preferred!=48000)rates[count++]=48000;
    aaudio_performance_mode_t modes[]={AAUDIO_PERFORMANCE_MODE_NONE,AAUDIO_PERFORMANCE_MODE_LOW_LATENCY};
    using SetAttr=void(*)(AAudioStreamBuilder*,int32_t);using GetAttr=int32_t(*)(AAudioStream*);
    auto usage=reinterpret_cast<SetAttr>(dlsym(RTLD_DEFAULT,"AAudioStreamBuilder_setUsage"));auto content=reinterpret_cast<SetAttr>(dlsym(RTLD_DEFAULT,"AAudioStreamBuilder_setContentType"));
    for(int r=0;r<count;++r)for(auto mode:modes) {
        AAudioStreamBuilder* b=nullptr;if(AAudio_createStreamBuilder(&b)!=AAUDIO_OK||!b)continue;
        AAudioStreamBuilder_setDirection(b,AAUDIO_DIRECTION_OUTPUT);AAudioStreamBuilder_setFormat(b,AAUDIO_FORMAT_PCM_I16);AAudioStreamBuilder_setChannelCount(b,2);
        AAudioStreamBuilder_setSampleRate(b,rates[r]);AAudioStreamBuilder_setSharingMode(b,AAUDIO_SHARING_MODE_SHARED);AAudioStreamBuilder_setPerformanceMode(b,mode);
        if(usage)usage(b,AAUDIO_USAGE_MEDIA);if(content)content(b,AAUDIO_CONTENT_TYPE_MUSIC);
        if(settings.capacity_ms>0)AAudioStreamBuilder_setBufferCapacityInFrames(b,rates[r]*settings.capacity_ms/1000);
        auto* stream=new Stream;stream->error=callback;stream->user=user;AAudioStreamBuilder_setErrorCallback(b,failed,stream);
        auto rc=AAudioStreamBuilder_openStream(b,&stream->handle);AAudioStreamBuilder_delete(b);
        if(rc!=AAUDIO_OK||!stream->handle){delete stream;continue;}
        if(AAudioStream_getSampleRate(stream->handle)<8000){AAudioStream_close(stream->handle);delete stream;continue;}
        int cap=AAudioStream_getBufferCapacityInFrames(stream->handle);
        if(cap>0&&settings.bursts>0)AAudioStream_setBufferSizeInFrames(stream->handle,std::min(cap,AAudioStream_getFramesPerBurst(stream->handle)*settings.bursts));
        auto get_usage=reinterpret_cast<GetAttr>(dlsym(RTLD_DEFAULT,"AAudioStream_getUsage"));auto get_content=reinterpret_cast<GetAttr>(dlsym(RTLD_DEFAULT,"AAudioStream_getContentType"));
        lyra_log(LyraLogInfo,"Lyra","play stream rate=%d burst=%d cap=%d size=%d mode=%d usage=%d content=%d",sample_rate(stream),burst(stream),cap,size(stream),int(mode),get_usage?get_usage(stream->handle):-1,get_content?get_content(stream->handle):-1);
        return stream;
    }
    return nullptr;
}
Result close(Stream* s){auto rc=AAudioStream_close(s->handle);if(rc==AAUDIO_OK)delete s;return rc;}
int sample_rate(Stream* s){return AAudioStream_getSampleRate(s->handle);}
int xruns(Stream* s){return AAudioStream_getXRunCount(s->handle);}
int capacity(Stream* s){return AAudioStream_getBufferCapacityInFrames(s->handle);}
int size(Stream* s){return AAudioStream_getBufferSizeInFrames(s->handle);}
int burst(Stream* s){return AAudioStream_getFramesPerBurst(s->handle);}
int set_size(Stream* s,int n){return AAudioStream_setBufferSizeInFrames(s->handle,n);}
int64_t written(Stream* s){return AAudioStream_getFramesWritten(s->handle);}
State state(Stream* s){return State(AAudioStream_getState(s->handle));}
Result wait_state(Stream* s,State from,State* to,int64_t timeout){aaudio_stream_state_t next=aaudio_stream_state_t(from);auto rc=AAudioStream_waitForStateChange(s->handle,aaudio_stream_state_t(from),&next,timeout);*to=State(next);return rc;}
Result start(Stream* s){return AAudioStream_requestStart(s->handle);}
Result pause(Stream* s){return AAudioStream_requestPause(s->handle);}
Result flush(Stream* s){return AAudioStream_requestFlush(s->handle);}
Result stop(Stream* s){return AAudioStream_requestStop(s->handle);}
Result write(Stream* s,const void* data,int frames,int64_t timeout){return AAudioStream_write(s->handle,data,frames,timeout);}
Result timestamp(Stream* s,int64_t* frame,int64_t* ns){return AAudioStream_getTimestamp(s->handle,CLOCK_MONOTONIC,frame,ns);}
}
