#pragma once
#include "plugin/lyra_plugin.h"
#include "plugin/stem_contracts.h"
#include <dlfcn.h>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <jni.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "platform/log.h"
#include <cerrno>
#ifdef __ANDROID__
#include <android/dlext.h>
#endif
// Parent-owned optional plugin dependency. It uses the same public entry and
// typed message endpoint as a standalone plugin; no private host callback.
struct StemDependency {
 std::string directory;void *handle=nullptr;LyraPluginV3 plugin{};const LyraHostV3 *host=nullptr;const LyraStemAnalysisV3 *api=nullptr;
 void close(){if(plugin.shutdown && host)plugin.shutdown(host);plugin={};api=nullptr;host=nullptr;if(handle)dlclose(handle);handle=nullptr;if(!directory.empty()){unlink((directory+"/plugin.so").c_str());unlink((directory+"/worker").c_str());unlink((directory+"/models/vocals.param").c_str());unlink((directory+"/models/vocals.bin").c_str());rmdir((directory+"/models").c_str());rmdir(directory.c_str());directory.clear();}}
 static std::string installed_root(const LyraHostV3 *host,const std::string &fallback){
  if(access((fallback+"/plugin.so").c_str(),R_OK)==0)return fallback;
  if(!host || !host->java_vm || !host->android_context)return fallback;
  auto *vm=static_cast<JavaVM*>(host->java_vm);JNIEnv *env=nullptr;bool attached=false;
  if(vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)==JNI_EDETACHED)attached=vm->AttachCurrentThread(&env,nullptr)==JNI_OK;
  if(!env)return fallback;std::string found=fallback;
  if(env->PushLocalFrame(16)==JNI_OK){auto activity=static_cast<jobject>(host->android_context);jclass cls=env->GetObjectClass(activity);
   for(int external=0;external<2;++external){jmethodID method=env->GetMethodID(cls,external?"getExternalFilesDir":"getFilesDir",external?"(Ljava/lang/String;)Ljava/io/File;":"()Ljava/io/File;");
    if(env->ExceptionCheck()){env->ExceptionClear();continue;}if(!method)continue;
    jobject file=external?env->CallObjectMethod(activity,method,nullptr):env->CallObjectMethod(activity,method);
    if(env->ExceptionCheck()){env->ExceptionClear();continue;}if(!file)continue;jclass type=env->GetObjectClass(file);jmethodID path=env->GetMethodID(type,"getAbsolutePath","()Ljava/lang/String;");if(env->ExceptionCheck()){env->ExceptionClear();continue;}
    if(!path)continue;auto text=static_cast<jstring>(env->CallObjectMethod(file,path));if(env->ExceptionCheck()){env->ExceptionClear();continue;}if(!text)continue;
    const char *utf=env->GetStringUTFChars(text,nullptr);if(utf){std::string candidate=std::string(utf)+"/lyra/plugins/official.transition-stems";env->ReleaseStringUTFChars(text,utf);if(access((candidate+"/plugin.so").c_str(),R_OK)==0)found=candidate;}if(env->ExceptionCheck())env->ExceptionClear();
   }env->PopLocalFrame(nullptr);
  }else if(env->ExceptionCheck())env->ExceptionClear();if(attached)vm->DetachCurrentThread();return found;
 }
 static bool copy(const std::string &from,const std::string &to){
  FILE *in=fopen(from.c_str(),"rb"),*out=fopen(to.c_str(),"wb");bool ok=in && out;char buffer[16384];
  if(ok)for(;;){size_t n=fread(buffer,1,sizeof(buffer),in);if(n && fwrite(buffer,1,n,out)!=n){ok=false;break;}if(n<sizeof(buffer)){ok=!ferror(in);break;}}
  if(in)fclose(in);if(out)ok=fclose(out)==0 && ok;if(ok)ok=chmod(to.c_str(),0400)==0;return ok;
 }
 bool load(const LyraHostV3 *value,const std::string &root){
  close();std::string source=installed_root(value,root),pattern=root+"/stems-XXXXXX";
  if(access((source+"/plugin.so").c_str(),R_OK))return false;
  std::vector<char> temp(pattern.begin(),pattern.end());temp.push_back(0);char *created=mkdtemp(temp.data());if(!created)return false;directory=created;
  // One immutable, content-addressed model file, shared by private module snapshots.
  // Force-stop may leave snapshots behind; the model is never duplicated per launch.
  std::string weights=root+"/.stem-vocals-401c9ca0845137852eebcd3961c5754fb7e502d995cc81d15ee5812620817bf9.bin";struct stat cached{};
  if(stat(weights.c_str(),&cached) || cached.st_size!=19660432){unlink(weights.c_str());if(!copy(source+"/models/vocals.bin",weights)){lyra_log(LyraLogError,"Lyra","stem weights copy failed source=%s errno=%d",source.c_str(),errno);close();return false;}}
  if(mkdir((directory+"/models").c_str(),0700) || !copy(source+"/plugin.so",directory+"/plugin.so") || !copy(source+"/worker",directory+"/worker") || !copy(source+"/models/vocals.param",directory+"/models/vocals.param")){close();return false;}
  std::string path=directory+"/plugin.so";handle=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);
#ifdef __ANDROID__
  if(!handle){int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC);if(fd>=0){android_dlextinfo info{};info.flags=ANDROID_DLEXT_USE_LIBRARY_FD;info.library_fd=fd;handle=android_dlopen_ext(path.c_str(),RTLD_NOW|RTLD_LOCAL,&info);::close(fd);}}
#endif
  if(!handle){lyra_log(LyraLogError,"Lyra","stem private module load failed: %s",dlerror());close();return false;}unlink(path.c_str());auto init=reinterpret_cast<int(*)(const LyraHostV3*,LyraPluginV3*)>(dlsym(handle,"lyra_plugin_init"));host=value;
  if(!init || init(host,&plugin) || plugin.size!=sizeof(plugin) || plugin.abi!=LYRA_PLUGIN_ABI || !plugin.init || !plugin.query_interface || plugin.init(host)){close();return false;}
  api=static_cast<const LyraStemAnalysisV3*>(plugin.query_interface(host,LYRA_INTERFACE_STEM_ANALYSIS,LYRA_INTERFACE_VERSION,sizeof(LyraStemAnalysisV3)));
  if(!api || api->size!=sizeof(*api) || api->version!=LYRA_INTERFACE_VERSION || !api->request){close();return false;}return true;
 }
};
