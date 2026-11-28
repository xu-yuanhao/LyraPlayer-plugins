#include "plugin/lyra_plugin.h"
#include "plugin/media_contracts.h"
#include "plugin/settings_contracts.h"
#include "plugin/store_contracts.h"
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
enum class DisplayMode : unsigned char { Original, Translation, Phonetic };
constexpr char mode_key[] = "display_mode";
constexpr char settings_title[] = "词幕";
constexpr int64_t settings_count = 2, settings_page = 0;
struct Plugin {
    const LyraHostV3 *host=nullptr;JavaVM *vm=nullptr;jobject bridge=nullptr;
    jmethodID document=nullptr,state=nullptr,stop=nullptr;
    ALooper *looper=nullptr;int timer=-1,period=0;
    int64_t track=-2,duration=-1;uint64_t revision=0;
    std::string title,artist,album;
    const LyraStoreInterfaceV3 *store=nullptr;
    DisplayMode mode=DisplayMode::Original;
    int64_t settings_revision=0, row_flags=0;
    bool document_dirty=false;
    LyraSettingsPageV3 settings{sizeof(settings),LYRA_INTERFACE_VERSION,this,settings_request};

    bool store_mode(bool write, DisplayMode &value) {
        if (!store) return false;
        LyraFieldV3 fields[] = {
            {LYRA_STORE_KEY, LYRA_FIELD_UTF8, 0, sizeof(mode_key), mode_key},
            {LYRA_STORE_VALUE, LYRA_FIELD_BYTES, 0, sizeof(value), &value},
        };
        LyraMessageV3 message{sizeof(message),write?2u:1u,
                              write?LYRA_STORE_SET:LYRA_STORE_GET,fields};
        LyraFieldV3 returned[2]{};
        LyraReplyV3 reply{sizeof(reply),2,0,0,returned};
        if (store->request(store->user,&message,&reply)!=LYRA_MESSAGE_OK || reply.status || reply.field_count>2)
            return false;
        if (write) return true;
        for (uint32_t i=0;i<reply.field_count;++i) {
            const auto &field=returned[i];
            if (field.tag!=LYRA_STORE_VALUE || field.type!=LYRA_FIELD_BYTES ||
                field.bytes!=sizeof(value) || !field.data) continue;
            DisplayMode previous;
            std::memcpy(&previous,field.data,sizeof(previous));
            if (previous<=DisplayMode::Phonetic) value=previous;
        }
        return true;
    }

    static int settings_request(void *user,const LyraMessageV3 *message,LyraReplyV3 *reply) {
        if (!reply || reply->size!=sizeof(*reply)) return LYRA_MESSAGE_INVALID;
        reply->field_count=0;
        const LyraFieldRuleV3 rules[]={{LYRA_SETTINGS_INDEX,LYRA_FIELD_I64},
                                     {LYRA_SETTINGS_SECONDARY,LYRA_FIELD_I64},
                                     {LYRA_SETTINGS_VALUE,LYRA_FIELD_F64}};
        auto result=[&](int status){return reply->status=status;};
        int status=lyra_message_fields(message,rules,std::size(rules));
        if (status) return result(status);
        auto &self=*static_cast<Plugin *>(user);
        const auto operation=message->operation;
        if (operation==LYRA_SETTINGS_OPEN) return result(LYRA_MESSAGE_OK);
        if (operation==LYRA_SETTINGS_INFO) {
            if (!reply->fields || reply->capacity<5) return result(LYRA_MESSAGE_CAPACITY);
            const LyraFieldV3 fields[]={
                {LYRA_SETTINGS_ENTRY_TITLE,LYRA_FIELD_UTF8,0,sizeof(settings_title),settings_title},
                {LYRA_SETTINGS_TITLE,LYRA_FIELD_UTF8,0,sizeof(settings_title),settings_title},
                {LYRA_SETTINGS_COUNT,LYRA_FIELD_I64,0,sizeof(settings_count),&settings_count},
                {LYRA_SETTINGS_PAGE,LYRA_FIELD_I64,0,sizeof(settings_page),&settings_page},
                {LYRA_SETTINGS_REVISION,LYRA_FIELD_I64,0,sizeof(self.settings_revision),&self.settings_revision}
            };
            std::copy(std::begin(fields),std::end(fields),reply->fields);
            reply->field_count=std::size(fields);
            return result(LYRA_MESSAGE_OK);
        }
        if (operation!=LYRA_SETTINGS_ROW && operation!=LYRA_SETTINGS_ACTIVATE)
            return result(LYRA_MESSAGE_UNSUPPORTED);
        int64_t index=-1, secondary=0;
        lyra_message_value(message,LYRA_SETTINGS_INDEX,&index,sizeof(index));
        lyra_message_value(message,LYRA_SETTINGS_SECONDARY,&secondary,sizeof(secondary));
        if (index<0 || index>=settings_count || secondary) return result(LYRA_MESSAGE_INVALID);
        const auto selected=index==0?DisplayMode::Translation:DisplayMode::Phonetic;
        if (operation==LYRA_SETTINGS_ACTIVATE) {
            auto next=self.mode==selected?DisplayMode::Original:selected;
            if (!self.store_mode(true,next)) return result(LYRA_MESSAGE_UNSUPPORTED);
            self.mode=next;
            ++self.settings_revision;
            self.document_dirty=true;
            if (self.timer>=0) self.sample();
            if (self.host->request_frame) self.host->request_frame(self.host->user);
            return result(LYRA_MESSAGE_OK);
        }
        if (!reply->fields || reply->capacity<2) return result(LYRA_MESSAGE_CAPACITY);
        static constexpr char translation[]="仅显示翻译", phonetic[]="仅显示音译";
        self.row_flags=LYRA_SETTINGS_TOGGLE | (self.mode==selected?LYRA_SETTINGS_CHECKED:0);
        if (index==settings_count-1) self.row_flags|=LYRA_SETTINGS_GROUP_END;
        reply->fields[0]={LYRA_SETTINGS_TITLE,LYRA_FIELD_UTF8,0,
                          uint32_t(index==0?sizeof(translation):sizeof(phonetic)),index==0?translation:phonetic};
        reply->fields[1]={LYRA_SETTINGS_FLAGS,LYRA_FIELD_I64,0,sizeof(self.row_flags),&self.row_flags};
        reply->field_count=2;
        return result(LYRA_MESSAGE_OK);
    }
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
        if(document_dirty || track!=s.track_id || revision!=rev || duration!=dur || title!=t || artist!=a || album!=b){
            Writer out;out.integer(0x4c595231,4);out.integer(s.track_id,8);out.integer(rev,8);out.integer(dur,8);
            out.text(t);out.text(a);out.text(b);size_t count_at=out.bytes.size();out.integer(0,4);int count=0;
            int n=lyric?l->count(l->user):0;
            if(n<0 || n>32768)out.valid=false;
            for(int i=0;i<n && out.valid;++i){
                LyraLyricLineV3 line{};line.size=sizeof(line);
                if(l->line(l->user,i,&line)!=0 || line.size!=sizeof(line) || (line.flags&2) || line.start_us<0 || line.end_us/1000<=line.start_us/1000)continue;
                std::string main=text(l,i,0);if(main.empty())continue;
                std::string translation=text(l,i,1), phonetic=text(l,i,2);
                bool replaced=false;
                if (mode!=DisplayMode::Original) {
                    auto &alternate=mode==DisplayMode::Translation?translation:phonetic;
                    if (!alternate.empty()) {
                        main=std::move(alternate);
                        replaced=true;
                    }
                    translation.clear();
                    phonetic.clear();
                }
                out.integer(line.start_us/1000,8);out.integer(line.end_us/1000,8);
                out.text(main);out.text(translation);out.text(phonetic);
                size_t words_at=out.bytes.size();out.integer(0,4);int words=0;
                if(line.word_count<0 || line.word_count>4096){out.valid=false;break;}
                for(int w=0;!replaced && w<line.word_count && out.valid;++w){
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
            if(!e->ExceptionCheck() && bytes){document_dirty=false;track=s.track_id;revision=rev;duration=dur;title=t;artist=a;album=b;}
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
    const auto *store=static_cast<const LyraStoreInterfaceV3 *>(
        h->query_interface(h->user,LYRA_INTERFACE_STORE,LYRA_INTERFACE_VERSION,sizeof(LyraStoreInterfaceV3)));
    if (store && store->size==sizeof(*store) && store->version==LYRA_INTERFACE_VERSION && store->request) {
        p->store=store;
        p->store_mode(false,p->mode);
    }
    if(!supported){instance=std::move(p);return 0;}
    p->looper=ALooper_forThread();if(!p->looper)return -1;ALooper_acquire(p->looper);
    p->timer=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK|TFD_CLOEXEC);
    if(p->timer<0 || ALooper_addFd(p->looper,p->timer,ALOOPER_POLL_CALLBACK,ALOOPER_EVENT_INPUT,Plugin::ready,p.get())!=1)return -1;
    p->arm(50);instance=std::move(p);return 0;
}
}
extern "C" __attribute__((visibility("default"))) int lyra_plugin_init(const LyraHostV3 *h,LyraPluginV3 *out){
    if(!h || h->size!=sizeof(*h) || h->abi!=LYRA_PLUGIN_ABI || !h->query_interface || !out)return -1;
    *out={};out->size=sizeof(*out);out->abi=LYRA_PLUGIN_ABI;out->id="official.lyricon";out->kind="lyricon-provider";out->version="1.1.0";
    out->query_interface=[](const LyraHostV3*,uint32_t id,uint32_t version,uint32_t size)->const void*{
        if (instance && instance->store && id==LYRA_INTERFACE_SETTINGS_PAGE &&
            version==LYRA_INTERFACE_VERSION && size==sizeof(LyraSettingsPageV3))
            return &instance->settings;
        return nullptr;
    };
    out->init=init;out->shutdown=[](const LyraHostV3 *){instance.reset();};return 0;
}
