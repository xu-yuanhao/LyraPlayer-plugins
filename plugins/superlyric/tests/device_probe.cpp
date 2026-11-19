#include "plugin/lyra_plugin.h"
#include "plugin/media_contracts.h"
#include <jni.h>
#include <dlfcn.h>
#include <android/looper.h>
#include <cstring>
#include <pthread.h>
struct Fixture {
 void *library=nullptr;LyraHostV3 host{};LyraPluginV3 plugin{};int phase=0;pthread_t owner=pthread_self();
 LyraPlaybackInterfaceV3 playback{};LyraLyricsInterfaceV3 lyrics{};LyraMediaContextV3 media{};
 static const void *query(void *u,uint32_t id,uint32_t version,uint32_t size){
  auto &f=*static_cast<Fixture *>(u);if(!pthread_equal(f.owner,pthread_self()) || version!=LYRA_INTERFACE_VERSION)return nullptr;
  if(id==LYRA_INTERFACE_PLAYBACK && size==sizeof(f.playback))return &f.playback;
  if(id==LYRA_INTERFACE_LYRICS && size==sizeof(f.lyrics))return &f.lyrics;
  if(id==LYRA_INTERFACE_MEDIA_CONTEXT && size==sizeof(f.media))return &f.media;
  return nullptr;
 }
 Fixture(){
  playback={sizeof(playback),LYRA_INTERFACE_VERSION,this,[](void *u,LyraPlaybackSnapshotV3 *s){auto &f=*static_cast<Fixture *>(u);if(s->size!=sizeof(*s))return -1;*s={};s->size=sizeof(*s);s->flags=f.phase==4?0:LYRA_PLAYBACK_ACTIVE|(f.phase==1?LYRA_PLAYBACK_PAUSED:0);s->track_id=f.phase>=3?2:1;s->duration_us=4000000;s->position_us=f.phase==2?2500000:1250000;s->seek_serial=f.phase>=2?1:0;return 0;},nullptr,nullptr};
  lyrics={sizeof(lyrics),LYRA_INTERFACE_VERSION,this,
   [](void *u)->uint64_t{return static_cast<Fixture *>(u)->phase>=3?2:1;},
   [](void *u)->int64_t{return static_cast<Fixture *>(u)->phase>=3?2:1;},
   [](void *){return 2;},
   [](void *,int i,LyraLyricLineV3 *l){if(i<0||i>1||l->size!=sizeof(*l))return -1;*l={};l->size=sizeof(*l);l->start_us=i*2000000;l->end_us=(i+1)*2000000;l->word_count=i==0?2:0;return 0;},
   [](void *u,int i,int kind,int part,char *out,int capacity){const char *value="";if(kind==0)value=i==0?(static_cast<Fixture *>(u)->phase>=3?"新歌 🎵":"你好 🎵"):"下一句";if(kind==1)value="translation";if(kind==2)value="ni hao";if(kind==4)value=part==0?"你好 ":"🎵";int n=int(strlen(value))+1;if(out && capacity>=n)memcpy(out,value,n);return n;},
   [](void *,int i,int w,LyraLyricWordV3 *out){if(i!=0||w<0||w>1||out->size!=sizeof(*out))return -1;*out={};out->size=sizeof(*out);out->start_us=w*1000000;out->end_us=(w+1)*1000000;return 0;}};
  media.size=sizeof(media);media.version=LYRA_INTERFACE_VERSION;media.user=this;
  media.current=[](void *u,LyraMediaDescriptorV3 *m){if(m->size!=sizeof(*m))return -1;*m={};m->size=sizeof(*m);m->title=static_cast<Fixture *>(u)->phase>=3?"second":"first";m->artist="artist";m->album="album";return 0;};
 }
};
extern "C" JNIEXPORT jlong JNICALL Java_org_lyraplayer_superlyrictest_DeviceProbe_open(JNIEnv *e,jclass,jobject context,jstring path){
 auto *f=new Fixture;const char *p=e->GetStringUTFChars(path,nullptr);f->library=dlopen(p,RTLD_NOW|RTLD_LOCAL);e->ReleaseStringUTFChars(path,p);if(!f->library){delete f;return 0;}
 auto init=reinterpret_cast<LyraPluginInitFn>(dlsym(f->library,"lyra_plugin_init"));JavaVM *vm=nullptr;e->GetJavaVM(&vm);ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
 f->host.size=sizeof(f->host);f->host.abi=LYRA_PLUGIN_ABI;f->host.user=f;f->host.java_vm=vm;f->host.android_context=context;f->host.query_interface=Fixture::query;
 if(!init||init(&f->host,&f->plugin)||f->plugin.init(&f->host)){if(f->plugin.shutdown)f->plugin.shutdown(&f->host);dlclose(f->library);delete f;return 0;}return reinterpret_cast<jlong>(f);
}
extern "C" JNIEXPORT void JNICALL Java_org_lyraplayer_superlyrictest_DeviceProbe_step(JNIEnv *,jclass,jlong pointer,jint phase){auto *f=reinterpret_cast<Fixture *>(pointer);f->phase=phase;int fd,events;void *data;ALooper_pollOnce(60,&fd,&events,&data);}
extern "C" JNIEXPORT void JNICALL Java_org_lyraplayer_superlyrictest_DeviceProbe_close(JNIEnv *,jclass,jlong pointer){auto *f=reinterpret_cast<Fixture *>(pointer);f->plugin.shutdown(&f->host);dlclose(f->library);delete f;}
