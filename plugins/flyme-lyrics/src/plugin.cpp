#include "plugin/lyra_plugin.h"
#include "timeline.h"
#include "bridge_dex.h"
#include <android/looper.h>
#include <jni.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <cstring>
#include <memory>

namespace {
struct Env {
    JavaVM *vm; JNIEnv *env = nullptr; bool attached = false;
    explicit Env(JavaVM *v):vm(v) {
        if (vm && vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_EDETACHED)
            attached = vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
    }
    ~Env() { if (attached) vm->DetachCurrentThread(); }
};
struct Plugin {
    const LyraHostV3 *host = nullptr;
    JavaVM *vm = nullptr;
    jobject bridge = nullptr;
    jmethodID set_cue = nullptr, stop = nullptr;
    ALooper *looper = nullptr;
    int timer = -1;
    uint64_t revision = 0;
    int64_t track = -1;
    std::vector<flyme::Cue> cues;
    std::string previous;
    ~Plugin() {
        if (looper) { ALooper_removeFd(looper, timer); ALooper_release(looper); }
        if (timer >= 0) close(timer);
        Env j(vm);
        if (bridge && j.env) {
            j.env->CallVoidMethod(bridge, stop);
            if (j.env->ExceptionCheck()) j.env->ExceptionClear();
            j.env->DeleteGlobalRef(bridge);
        }
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
            jstring name = e->NewStringUTF("org.lyraplayer.flymelyrics.NotificationBridge");
            auto klass = static_cast<jclass>(e->CallObjectMethod(loader,load,name));
            if (!klass || e->ExceptionCheck()) break;
            jmethodID create = e->GetMethodID(klass,"<init>","(Landroid/content/Context;)V");
            set_cue=e->GetMethodID(klass,"setCue","([B)V");
            stop=e->GetMethodID(klass,"stop","()V");
            jmethodID supported=e->GetMethodID(klass,"supported","()Z");
            if (!create || !set_cue || !stop || !supported || e->ExceptionCheck()) break;
            jobject value=e->NewObject(klass,create,context);
            if (!value || e->ExceptionCheck()) break;
            bridge=e->NewGlobalRef(value);
            if(bridge)ready=e->CallBooleanMethod(value,supported)?1:0;
        } while(false);
        if (e->ExceptionCheck()) {e->ExceptionClear();ready=-1;}
        e->PopLocalFrame(nullptr);
        return ready;
    }
    void sample() {
        // Resolve borrowed interfaces on the owner looper each time, never from
        // the notification worker or across plugin/provider replacement.
        auto *p=static_cast<const LyraPlaybackInterfaceV3 *>(host->query_interface(host->user,LYRA_INTERFACE_PLAYBACK,LYRA_INTERFACE_VERSION,sizeof(LyraPlaybackInterfaceV3)));
        auto *l=static_cast<const LyraLyricsInterfaceV3 *>(host->query_interface(host->user,LYRA_INTERFACE_LYRICS,LYRA_INTERFACE_VERSION,sizeof(LyraLyricsInterfaceV3)));
        LyraPlaybackSnapshotV3 state{};state.size=sizeof(state);
        const char *text="";
        if (p && p->size==sizeof(*p) && p->version==LYRA_INTERFACE_VERSION && p->snapshot &&
            l && l->size==sizeof(*l) && l->version==LYRA_INTERFACE_VERSION && l->track_id && l->revision && l->count && l->line && l->text &&
            p->snapshot(p->user,&state)==0 && state.size==sizeof(state) &&
            (state.flags&LYRA_PLAYBACK_ACTIVE) && !(state.flags&(LYRA_PLAYBACK_PAUSED|LYRA_PLAYBACK_SEEKING)) &&
            state.track_id==l->track_id(l->user)) {
            uint64_t next=l->revision(l->user);
            if (track!=state.track_id || revision!=next) {
                cues.clear();track=state.track_id;revision=next;
                int n=l->count(l->user);size_t bytes=0;
                if (n>=0 && n<=32768) for(int i=0;i<n;++i) {
                    LyraLyricLineV3 line{};line.size=sizeof(line);
                    if(l->line(l->user,i,&line)!=0 || line.size!=sizeof(line) || (line.flags&2) || line.end_us<=line.start_us)continue;
                    int size=l->text(l->user,i,0,0,nullptr,0);
                    if(size<=1 || size>65536 || (bytes+=size)>4*1024*1024)continue;
                    std::string value(size,'\0');
                    if(l->text(l->user,i,0,0,value.data(),size)!=size || value.back()!=0)continue;
                    value.resize(size-1);cues.push_back({line.start_us,line.end_us,std::move(value)});
                }
            }
            text=flyme::current(cues,state.position_us);
        }
        if(previous==text)return;
        std::string next=text;
        Env j(vm);if(!j.env)return;
        // Pass standard UTF-8 bytes to the SDK, including supplementary glyphs.
        auto *e=j.env;if(e->PushLocalFrame(4)!=JNI_OK)return;
        jbyteArray bytes=e->NewByteArray(jint(next.size()));
        if(bytes)e->SetByteArrayRegion(bytes,0,jint(next.size()),reinterpret_cast<const jbyte *>(next.data()));
        if(bytes && !e->ExceptionCheck())e->CallVoidMethod(bridge,set_cue,bytes);
        if(e->ExceptionCheck())e->ExceptionClear();
        else if(bytes)previous=std::move(next);
        e->PopLocalFrame(nullptr);
    }
    static int ready(int fd,int events,void *user) {
        if(!(events&ALOOPER_EVENT_INPUT))return 1;
        uint64_t ticks;if(read(fd,&ticks,sizeof(ticks))==sizeof(ticks))static_cast<Plugin *>(user)->sample();
        return 1;
    }
};
std::unique_ptr<Plugin> instance;
int init(const LyraHostV3 *h) {
    auto p=std::make_unique<Plugin>();p->host=h;
    int supported=p->java(h);
    if(supported<0)return -1;
    if(!supported){instance=std::move(p);return 0;} // Unsupported devices stay inert.
    p->looper=ALooper_forThread();
    if(!p->looper)return -1;
    ALooper_acquire(p->looper);
    p->timer=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK|TFD_CLOEXEC);
    if(p->timer<0 || ALooper_addFd(p->looper,p->timer,ALOOPER_POLL_CALLBACK,ALOOPER_EVENT_INPUT,Plugin::ready,p.get())!=1)return -1;
    itimerspec spec{};spec.it_value.tv_nsec=1;spec.it_interval.tv_nsec=250000000;
    if(timerfd_settime(p->timer,0,&spec,nullptr)!=0)return -1;
    instance=std::move(p);return 0;
}
}
extern "C" __attribute__((visibility("default"))) int lyra_plugin_init(const LyraHostV3 *h,LyraPluginV3 *out) {
    if(!h || h->size!=sizeof(*h) || h->abi!=LYRA_PLUGIN_ABI || !h->query_interface || !out)return -1;
    *out={};out->size=sizeof(*out);out->abi=LYRA_PLUGIN_ABI;
    out->id="official.flyme-lyrics";out->kind="statusbar-lyrics";out->version="1.0.0";
    out->init=init;out->shutdown=[](const LyraHostV3 *){instance.reset();};return 0;
}
