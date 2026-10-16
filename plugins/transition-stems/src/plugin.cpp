#include "process_model.h"
#ifndef LYRA_PLUGIN_VERSION
#error "Build must provide the catalog plugin version"
#endif
#include <dlfcn.h>
#include <cstring>
#include "plugin/contracts.h"
#include "plugin/lyra_plugin.h"
#include <jni.h>
#include <mutex>
#include <memory>
#include <sys/stat.h>
#include <cstdio>
#include <vector>
namespace {
struct Env {
 JavaVM *vm=nullptr;JNIEnv *env=nullptr;bool attached=false;
 explicit Env(JavaVM *v):vm(v){if(vm && vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)==JNI_EDETACHED){attached=vm->AttachCurrentThread(&env,nullptr)==JNI_OK;}if(env && env->PushLocalFrame(32)!=JNI_OK){if(env->ExceptionCheck())env->ExceptionClear();env=nullptr;}}
 ~Env(){if(env && env->ExceptionCheck())env->ExceptionClear();if(env)env->PopLocalFrame(nullptr);if(attached)vm->DetachCurrentThread();}
 bool ok(){if(!env)return false;if(env->ExceptionCheck()){env->ExceptionClear();return false;}return true;}
};
bool copy_asset(const std::string &from,const std::string &to){
 FILE *in=fopen(from.c_str(),"rb"),*out=fopen(to.c_str(),"wb");bool ok=in && out;char bytes[16384];
 if(ok)for(;;){size_t n=fread(bytes,1,sizeof(bytes),in);if(n && fwrite(bytes,1,n,out)!=n){ok=false;break;}if(n<sizeof(bytes)){ok=!ferror(in);break;}}
 if(in)fclose(in);if(out)ok=fclose(out)==0 && ok;return ok && chmod(to.c_str(),0400)==0;
}
struct State {std::mutex mutex;lyra_stems::ProcessModel model;std::string root,weights,owned_root;JavaVM *vm=nullptr;jobject activity=nullptr;std::vector<int64_t> library;int64_t fd=-1;
 ~State(){model.release();if(!owned_root.empty()){unlink((owned_root+"/worker").c_str());unlink((owned_root+"/models/vocals.param").c_str());rmdir((owned_root+"/models").c_str());rmdir(owned_root.c_str());}}
};
bool prepare_assets(State &s){
 if(!access((s.root+"/worker").c_str(),R_OK) && !access((s.root+"/models/vocals.param").c_str(),R_OK))return true;
 Env e(s.vm);if(!e.ok() || !s.activity)return false;std::string source;
 jclass cls=e.env->GetObjectClass(s.activity);if(!e.ok() || !cls)return false;
 for(bool external : {false,true}){
  jmethodID get=e.env->GetMethodID(cls,external?"getExternalFilesDir":"getFilesDir",external?"(Ljava/lang/String;)Ljava/io/File;":"()Ljava/io/File;");if(!e.ok() || !get)continue;
  jobject file=external?e.env->CallObjectMethod(s.activity,get,nullptr):e.env->CallObjectMethod(s.activity,get);if(!e.ok() || !file)continue;
  jclass type=e.env->GetObjectClass(file);jmethodID path=e.env->GetMethodID(type,"getAbsolutePath","()Ljava/lang/String;");if(!e.ok() || !path)continue;
  auto text=static_cast<jstring>(e.env->CallObjectMethod(file,path));if(!e.ok() || !text)continue;
  const char *utf=e.env->GetStringUTFChars(text,nullptr);if(!utf || !e.ok())continue;
  std::string candidate=std::string(utf)+"/lyra/plugins/official.transition-stems";e.env->ReleaseStringUTFChars(text,utf);
  if(!access((candidate+"/worker").c_str(),R_OK))source=candidate;
 }
 if(source.empty())return false;
 std::string weights=s.root+"/.stem-vocals-401c9ca0845137852eebcd3961c5754fb7e502d995cc81d15ee5812620817bf9.bin";struct stat st{};
 if(stat(weights.c_str(),&st) || st.st_size!=19660432){unlink(weights.c_str());if(!copy_asset(source+"/models/vocals.bin",weights))return false;}
 std::string pattern=s.root+"/stem-assets-XXXXXX";std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);char *dir=mkdtemp(name.data());if(!dir)return false;s.owned_root=dir;
 if(mkdir((s.owned_root+"/models").c_str(),0700) || !copy_asset(source+"/worker",s.owned_root+"/worker") || !copy_asset(source+"/models/vocals.param",s.owned_root+"/models/vocals.param"))return false;
 s.root=s.owned_root;s.weights=weights;s.model.executable(s.root+"/worker");return true;
}
std::unique_ptr<State> state;
int init(const LyraHostV3 *host){state=std::make_unique<State>();Dl_info path{};if(!dladdr(reinterpret_cast<void*>(&init),&path) || !path.dli_fname)return -1;state->root=path.dli_fname;state->root.resize(state->root.find_last_of('/'));state->model.executable(state->root+"/worker");state->weights=state->root+"/models/vocals.bin";
 if(access(state->weights.c_str(),R_OK))state->weights=state->root.substr(0,state->root.find_last_of('/'))+"/.stem-vocals-401c9ca0845137852eebcd3961c5754fb7e502d995cc81d15ee5812620817bf9.bin";
 if(host){state->vm=static_cast<JavaVM*>(host->java_vm);Env env(state->vm);if(env.env && host->android_context)state->activity=env.env->NewGlobalRef(static_cast<jobject>(host->android_context));}return 0;}
void shutdown(const LyraHostV3*){if(state && state->activity){Env env(state->vm);if(env.env)env.env->DeleteGlobalRef(state->activity);}state.reset();}
bool read_i64(const LyraMessageV3 *message,uint32_t tag,int64_t *value){if(message->field_count!=1 || !message->fields)return false;const auto &f=message->fields[0];if(f.tag!=tag || f.type!=LYRA_FIELD_I64 || f.bytes!=sizeof(*value) || !f.data)return false;memcpy(value,f.data,sizeof(*value));return true;}
int request(void*,const LyraMessageV3 *message,LyraReplyV3 *reply){
 if(!state || !message || message->size!=sizeof(*message) || !reply || reply->size!=sizeof(*reply))return LYRA_MESSAGE_INVALID;reply->field_count=0;reply->status=LYRA_MESSAGE_INVALID;std::lock_guard<std::mutex> lock(state->mutex);
 auto finish=[&](int code){reply->status=code;return code;};
 if(message->operation==LYRA_STEM_RELEASE){if(message->field_count)return finish(LYRA_MESSAGE_INVALID);state->model.release();return finish(LYRA_MESSAGE_OK);}
 if(message->operation==LYRA_STEM_INFO){
  if(message->field_count)return finish(LYRA_MESSAGE_INVALID);if(reply->capacity<5 || !reply->fields)return finish(LYRA_MESSAGE_CAPACITY);
  static const int64_t rate=LYRA_STEM_RATE,frames=LYRA_STEM_INPUT_FRAMES,hop=LYRA_STEM_OUTPUT_HOP,context=LYRA_STEM_CONTEXT;static const char model[]="spleeter-2s-vocals-ncnn-fp16-64-v1";
  reply->fields[0]={LYRA_STEM_SAMPLE_RATE,LYRA_FIELD_I64,0,sizeof(rate),&rate};reply->fields[1]={LYRA_STEM_FRAME_COUNT,LYRA_FIELD_I64,0,sizeof(frames),&frames};reply->fields[2]={LYRA_STEM_HOP_FRAMES,LYRA_FIELD_I64,0,sizeof(hop),&hop};reply->fields[3]={LYRA_STEM_CONTEXT_FRAMES,LYRA_FIELD_I64,0,sizeof(context),&context};reply->fields[4]={LYRA_STEM_MODEL,LYRA_FIELD_UTF8,0,sizeof(model)-1,model};reply->field_count=5;return finish(LYRA_MESSAGE_OK);
 }
 if(message->operation==LYRA_STEM_PROCESS){
  LyraFieldRuleV3 rule{LYRA_STEM_PCM,LYRA_FIELD_BYTES};if(lyra_message_fields(message,&rule,1)<0 || message->field_count!=1 || message->fields[0].tag!=LYRA_STEM_PCM || !message->fields[0].data || message->fields[0].bytes!=2*LYRA_STEM_INPUT_FRAMES*sizeof(float))return finish(LYRA_MESSAGE_INVALID);
  if(reply->capacity<1 || !reply->fields)return finish(LYRA_MESSAGE_CAPACITY);
  if(!prepare_assets(*state) || !state->model.load(state->root+"/models/vocals.param",state->weights,1) || !state->model.process(static_cast<const float*>(message->fields[0].data),2*LYRA_STEM_INPUT_FRAMES))return finish(LYRA_MESSAGE_UNSUPPORTED);
  const auto &out=state->model.output();reply->fields[0]={LYRA_STEM_VOCALS,LYRA_FIELD_BYTES,0,uint32_t(out.size()*sizeof(float)),out.data()};reply->field_count=1;return finish(LYRA_MESSAGE_OK);
 }
 if(message->operation==LYRA_STEM_OPEN_SOURCE){
  int64_t id;if(!read_i64(message,LYRA_STEM_TRACK_ID,&id) || id<0)return finish(LYRA_MESSAGE_INVALID);if(reply->capacity<1 || !reply->fields)return finish(LYRA_MESSAGE_CAPACITY);if(!state->activity)return finish(LYRA_MESSAGE_UNSUPPORTED);
  Env env(state->vm);if(!env.ok())return finish(LYRA_MESSAGE_UNSUPPORTED);jclass cls=env.env->GetObjectClass(state->activity);if(!cls || !env.ok())return finish(LYRA_MESSAGE_UNSUPPORTED);jmethodID open=env.env->GetMethodID(cls,"openReadFd","(J)I");if(!open || !env.ok())return finish(LYRA_MESSAGE_UNSUPPORTED);state->fd=env.env->CallIntMethod(state->activity,open,jlong(id));if(!env.ok() || state->fd<0)return finish(LYRA_MESSAGE_UNSUPPORTED);
  reply->fields[0]={LYRA_STEM_FD,LYRA_FIELD_I64,0,sizeof(state->fd),&state->fd};reply->field_count=1;return finish(LYRA_MESSAGE_OK);
 }
 if(message->operation==LYRA_STEM_LIBRARY){
  int64_t maximum;if(!read_i64(message,LYRA_STEM_LIBRARY_LIMIT,&maximum) || maximum<1 || maximum>16384)return finish(LYRA_MESSAGE_INVALID);if(reply->capacity<1 || !reply->fields)return finish(LYRA_MESSAGE_CAPACITY);if(!state->activity)return finish(LYRA_MESSAGE_UNSUPPORTED);
  Env e(state->vm);if(!e.ok())return finish(LYRA_MESSAGE_UNSUPPORTED);auto *env=e.env;jclass activity=env->GetObjectClass(state->activity);jmethodID get=env->GetMethodID(activity,"getContentResolver","()Landroid/content/ContentResolver;");if(!get || !e.ok())return finish(LYRA_MESSAGE_UNSUPPORTED);jobject resolver=env->CallObjectMethod(state->activity,get);if(!e.ok() || !resolver)return finish(LYRA_MESSAGE_UNSUPPORTED);
  jclass uriClass=env->FindClass("android/net/Uri");if(!e.ok() || !uriClass)return finish(LYRA_MESSAGE_UNSUPPORTED);jmethodID parse=env->GetStaticMethodID(uriClass,"parse","(Ljava/lang/String;)Landroid/net/Uri;");if(!e.ok() || !parse)return finish(LYRA_MESSAGE_UNSUPPORTED);jstring address=env->NewStringUTF("content://media/external/audio/media");jobject uri=env->CallStaticObjectMethod(uriClass,parse,address);
  if(!e.ok() || !uri)return finish(LYRA_MESSAGE_UNSUPPORTED);jclass string=env->FindClass("java/lang/String");if(!e.ok() || !string)return finish(LYRA_MESSAGE_UNSUPPORTED);jobjectArray columns=env->NewObjectArray(2,string,nullptr);jstring id=env->NewStringUTF("_id"),duration=env->NewStringUTF("duration");if(!e.ok() || !columns || !id || !duration)return finish(LYRA_MESSAGE_UNSUPPORTED);env->SetObjectArrayElement(columns,0,id);env->SetObjectArrayElement(columns,1,duration);
  if(!e.ok())return finish(LYRA_MESSAGE_UNSUPPORTED);jclass resolverClass=env->GetObjectClass(resolver);if(!e.ok() || !resolverClass)return finish(LYRA_MESSAGE_UNSUPPORTED);jmethodID query=env->GetMethodID(resolverClass,"query","(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;");jstring selection=env->NewStringUTF("is_music != 0"),sort=env->NewStringUTF("date_modified DESC");
  if(!query || !e.ok())return finish(LYRA_MESSAGE_UNSUPPORTED);jobject cursor=env->CallObjectMethod(resolver,query,uri,columns,selection,nullptr,sort);if(!e.ok() || !cursor)return finish(LYRA_MESSAGE_UNSUPPORTED);
  jclass cursorClass=env->GetObjectClass(cursor);jmethodID next=env->GetMethodID(cursorClass,"moveToNext","()Z"),read=env->GetMethodID(cursorClass,"getLong","(I)J"),close=env->GetMethodID(cursorClass,"close","()V");state->library.clear();bool ok=next && read && close && e.ok();
  if(ok)while(state->library.size()/2<size_t(maximum) && env->CallBooleanMethod(cursor,next)){int64_t track=env->CallLongMethod(cursor,read,0),ms=env->CallLongMethod(cursor,read,1);if(!e.ok()){ok=false;break;}if(track>=0 && ms>0){state->library.push_back(track);state->library.push_back(ms);}}
  if(close)env->CallVoidMethod(cursor,close);if(!ok || !e.ok())return finish(LYRA_MESSAGE_UNSUPPORTED);
  reply->fields[0]={LYRA_STEM_LIBRARY_ROWS,LYRA_FIELD_BYTES,0,uint32_t(state->library.size()*sizeof(int64_t)),state->library.data()};reply->field_count=1;return finish(LYRA_MESSAGE_OK);
 }
 return finish(LYRA_MESSAGE_UNSUPPORTED);
}
LyraStemAnalysisV3 api{sizeof(api),LYRA_INTERFACE_VERSION,nullptr,request};
const void *query(const LyraHostV3*,uint32_t id,uint32_t version,uint32_t size){return id==LYRA_INTERFACE_STEM_ANALYSIS && version==LYRA_INTERFACE_VERSION && size==sizeof(api)?&api:nullptr;}
}
extern "C" __attribute__((visibility("default"))) int lyra_plugin_init(const LyraHostV3 *host,LyraPluginV3 *out){if(!host || host->size!=sizeof(*host) || host->abi!=LYRA_PLUGIN_ABI || !out)return -1;*out={};out->size=sizeof(*out);out->abi=LYRA_PLUGIN_ABI;out->id="official.transition-stems";out->kind="audio-analysis";out->version=LYRA_PLUGIN_VERSION;out->init=init;out->shutdown=shutdown;out->query_interface=query;return 0;}
