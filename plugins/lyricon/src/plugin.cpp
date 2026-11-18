#include "plugin/lyra_plugin.h"
#include "plugin/media_contracts.h"
#include "bridge_dex.h"
#include <android/looper.h>
#include <jni.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {
struct Env {
    JavaVM *vm; JNIEnv *env=nullptr;bool attached=false;
    explicit Env(JavaVM *v):vm(v){if(vm && vm->GetEnv(reinterpret_cast<void **>(&env),JNI_VERSION_1_6)==JNI_EDETACHED)attached=vm->AttachCurrentThread(&env,nullptr)==JNI_OK;}
    ~Env(){if(attached)vm->DetachCurrentThread();}
};
struct Writer {
    std::vector<unsigned char> bytes;bool valid=true;
    void integer(uint64_t v,int n){if(bytes.size()+n>4*1024*1024){valid=false;return;}for(int i=n-1;i>=0;--i)bytes.push_back((v>>(i*8))&255);}
    void text(const std::string &s){if(s.size()>65536 || bytes.size()+s.size()+4>4*1024*1024){valid=false;return;}integer(s.size(),4);bytes.insert(bytes.end(),s.begin(),s.end());}
    void patch(size_t at,int n){if(at+4<=bytes.size())for(int i=0;i<4;++i)bytes[at+i]=(uint32_t(n)>>(24-i*8))&255;}
};
std::string text(const LyraLyricsInterfaceV3 *l,int line,int kind,int part=0){
    int n=l->text(l->user,line,kind,part,nullptr,0);if(n<=1 || n>65536)return {};
    std::string s(n,0);if(l->text(l->user,line,kind,part,s.data(),n)!=n || s.back()!=0)return {};s.pop_back();return s;
}
struct Plugin {
    const LyraHostV3 *host=nullptr;JavaVM *vm=nullptr;jobject bridge=nullptr;
    jmethodID document=nullptr,state=nullptr,stop=nullptr;
    ALooper *looper=nullptr;int timer=-1,period=0;
    int64_t track=-2,duration=-1;uint64_t revision=0;
    std::string title,artist,album;
    ~Plugin(){
        if(looper){ALooper_removeFd(looper,timer);ALooper_release(looper);}
        if(timer>=0)close(timer);
        Env j(vm);if(bridge && j.env){j.env->CallVoidMethod(bridge,stop);if(j.env->ExceptionCheck())j.env->ExceptionClear();j.env->DeleteGlobalRef(bridge);}
    }
    int java(const LyraHostV3 *h) {
        vm = static_cast<JavaVM *>(h->java_vm);
        Env j(vm); auto *e = j.env;
        if (!e || !h->android_context || e->PushLocalFrame(32) != JNI_OK) return -1;
        int ready = -1;
        do {
            jobject context = static_cast<jobject>(h->android_context);
            jclass context_class = e->GetObjectClass(context);
            jmethodID get_loader = e->GetMethodID(context_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
            if (!get_loader) break;
            jobject parent = e->CallObjectMethod(context, get_loader);
            jclass buffer_class = e->FindClass("java/nio/ByteBuffer");
            jmethodID allocate = buffer_class ? e->GetStaticMethodID(buffer_class,"allocateDirect","(I)Ljava/nio/ByteBuffer;") : nullptr;
            if (!allocate || e->ExceptionCheck()) break;
            jobject buffer = e->CallStaticObjectMethod(buffer_class, allocate, jint(sizeof(bridge_dex)));
            if (!buffer || e->ExceptionCheck()) break;
            void *data = e->GetDirectBufferAddress(buffer);
            if (!data) break;
            std::memcpy(data, bridge_dex, sizeof(bridge_dex));
            jclass loader_class = e->FindClass("dalvik/system/InMemoryDexClassLoader");
            jmethodID ctor = loader_class ? e->GetMethodID(loader_class,"<init>","(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V") : nullptr;
            if (!ctor || e->ExceptionCheck()) break;
            jobject loader = e->NewObject(loader_class,ctor,buffer,parent);
            if (!loader || e->ExceptionCheck()) break;
            jmethodID load = e->GetMethodID(loader_class,"loadClass","(Ljava/lang/String;)Ljava/lang/Class;");
            if (!load) break;
            jstring name = e->NewStringUTF("org.lyraplayer.lyricon.Endpoint");
            auto klass = static_cast<jclass>(e->CallObjectMethod(loader,load,name));
            if (!klass || e->ExceptionCheck()) break;
            jmethodID create = e->GetMethodID(klass,"<init>","(Landroid/content/Context;)V");
            document=e->GetMethodID(klass,"setDocument","([B)V");
            state=e->GetMethodID(klass,"setState","(JJJZ)I");
            stop=e->GetMethodID(klass,"stop","()V");
            jmethodID supported=e->GetMethodID(klass,"supported","()Z");
            if (!create || !document || !state || !stop || !supported || e->ExceptionCheck()) break;
            jobject value=e->NewObject(klass,create,context);
            if (!value || e->ExceptionCheck()) break;
            bridge=e->NewGlobalRef(value);
            if(bridge)ready=e->CallBooleanMethod(value,supported)?1:0;
        } while(false);
        if (e->ExceptionCheck()) {e->ExceptionClear();ready=-1;}
        e->PopLocalFrame(nullptr);
        return ready;
    }
    void arm(int ms){
        if(period==ms)return;period=ms;itimerspec spec{};spec.it_value.tv_sec=ms/1000;spec.it_value.tv_nsec=(ms%1000)*1000000L;spec.it_interval=spec.it_value;timerfd_settime(timer,0,&spec,nullptr);
    }
    void sample(){
        auto *p=static_cast<const LyraPlaybackInterfaceV3 *>(host->query_interface(host->user,LYRA_INTERFACE_PLAYBACK,LYRA_INTERFACE_VERSION,sizeof(LyraPlaybackInterfaceV3)));
        auto *l=static_cast<const LyraLyricsInterfaceV3 *>(host->query_interface(host->user,LYRA_INTERFACE_LYRICS,LYRA_INTERFACE_VERSION,sizeof(LyraLyricsInterfaceV3)));
        auto *m=static_cast<const LyraMediaContextV3 *>(host->query_interface(host->user,LYRA_INTERFACE_MEDIA_CONTEXT,LYRA_INTERFACE_VERSION,sizeof(LyraMediaContextV3)));
        LyraPlaybackSnapshotV3 s{};s.size=sizeof(s);s.track_id=-1;
        bool active=p && p->size==sizeof(*p) && p->version==LYRA_INTERFACE_VERSION && p->snapshot && p->snapshot(p->user,&s)==0 && s.size==sizeof(s) && (s.flags&LYRA_PLAYBACK_ACTIVE);
        if(!active)s.track_id=-1;
        bool lyric=active && l && l->size==sizeof(*l) && l->version==LYRA_INTERFACE_VERSION && l->track_id && l->revision && l->count && l->line && l->text && l->word && l->track_id(l->user)==s.track_id;
        LyraMediaDescriptorV3 meta{};meta.size=sizeof(meta);
        if(!m || m->size!=sizeof(*m) || m->version!=LYRA_INTERFACE_VERSION || !m->current || m->current(m->user,&meta)!=0 || meta.size!=sizeof(meta))meta={};
        const char *t=meta.title?meta.title:"",*a=meta.artist?meta.artist:"",*b=meta.album?meta.album:"";
        uint64_t rev=lyric?l->revision(l->user):0;int64_t dur=std::max<int64_t>(0,s.duration_us/1000);
        Env j(vm);auto *e=j.env;if(!e || e->PushLocalFrame(8)!=JNI_OK)return;
        if(track!=s.track_id || revision!=rev || duration!=dur || title!=t || artist!=a || album!=b){
            Writer out;out.integer(0x4c595231,4);out.integer(s.track_id,8);out.integer(rev,8);out.integer(dur,8);
            out.text(t);out.text(a);out.text(b);size_t count_at=out.bytes.size();out.integer(0,4);int count=0;
            int n=lyric?l->count(l->user):0;
            if(n<0 || n>32768)out.valid=false;
            for(int i=0;i<n && out.valid;++i){
                LyraLyricLineV3 line{};line.size=sizeof(line);
                if(l->line(l->user,i,&line)!=0 || line.size!=sizeof(line) || (line.flags&2) || line.start_us<0 || line.end_us/1000<=line.start_us/1000)continue;
                std::string main=text(l,i,0);if(main.empty())continue;
                out.integer(line.start_us/1000,8);out.integer(line.end_us/1000,8);out.text(main);out.text(text(l,i,1));out.text(text(l,i,2));
                size_t words_at=out.bytes.size();out.integer(0,4);int words=0;
                if(line.word_count<0 || line.word_count>4096){out.valid=false;break;}
                for(int w=0;w<line.word_count && out.valid;++w){
                    LyraLyricWordV3 word{};word.size=sizeof(word);
                    if(l->word(l->user,i,w,&word)!=0 || word.size!=sizeof(word) || word.start_us<0 || word.end_us/1000<=word.start_us/1000)continue;
                    std::string value=text(l,i,4,w);if(value.empty())continue;
                    out.integer(word.start_us/1000,8);out.integer(word.end_us/1000,8);out.text(value);++words;
                }
                out.patch(words_at,words);++count;
            }
            out.patch(count_at,count);
            // Reject an oversized document as a whole, never publish a partial
            // timeline under the new track's identity.
            if(!out.valid){out=Writer{};out.integer(0x4c595231,4);out.integer(s.track_id,8);out.integer(rev,8);out.integer(dur,8);out.text("");out.text("");out.text("");out.integer(0,4);}
            auto bytes=e->NewByteArray(jint(out.bytes.size()));
            if(bytes)e->SetByteArrayRegion(bytes,0,jint(out.bytes.size()),reinterpret_cast<const jbyte *>(out.bytes.data()));
            if(bytes && !e->ExceptionCheck())e->CallVoidMethod(bridge,document,bytes);
            if(!e->ExceptionCheck() && bytes){track=s.track_id;revision=rev;duration=dur;title=t;artist=a;album=b;}
        }
        if(!e->ExceptionCheck()){
            int ms=e->CallIntMethod(bridge,state,jlong(s.track_id),jlong(std::max<int64_t>(0,s.position_us/1000)),jlong(s.seek_serial),jboolean(active && !(s.flags&LYRA_PLAYBACK_PAUSED)));
            if(!e->ExceptionCheck())arm(ms==50 || ms==250?ms:1000);
        }
        if(e->ExceptionCheck())e->ExceptionClear();e->PopLocalFrame(nullptr);
    }
    static int ready(int fd,int events,void *user){if(events&ALOOPER_EVENT_INPUT){uint64_t n;if(read(fd,&n,sizeof(n))==sizeof(n))static_cast<Plugin *>(user)->sample();}return 1;}
};
std::unique_ptr<Plugin> instance;
int init(const LyraHostV3 *h){
    auto p=std::make_unique<Plugin>();p->host=h;int supported=p->java(h);if(supported<0)return -1;
    if(!supported){instance=std::move(p);return 0;}
    p->looper=ALooper_forThread();if(!p->looper)return -1;ALooper_acquire(p->looper);
    p->timer=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK|TFD_CLOEXEC);
    if(p->timer<0 || ALooper_addFd(p->looper,p->timer,ALOOPER_POLL_CALLBACK,ALOOPER_EVENT_INPUT,Plugin::ready,p.get())!=1)return -1;
    p->arm(50);instance=std::move(p);return 0;
}
}
extern "C" __attribute__((visibility("default"))) int lyra_plugin_init(const LyraHostV3 *h,LyraPluginV3 *out){
    if(!h || h->size!=sizeof(*h) || h->abi!=LYRA_PLUGIN_ABI || !h->query_interface || !out)return -1;
    *out={};out->size=sizeof(*out);out->abi=LYRA_PLUGIN_ABI;out->id="official.lyricon";out->kind="lyricon-provider";out->version="1.0.0";
    out->init=init;out->shutdown=[](const LyraHostV3 *){instance.reset();};return 0;
}
