#include "plugin/lyra_plugin.h"
#include <jni.h>
#include <dlfcn.h>
#include <android/looper.h>
static const void *query(void *,uint32_t,uint32_t,uint32_t){return nullptr;}
extern "C" JNIEXPORT jint JNICALL Java_org_lyraplayer_flymelyrics_DeviceProbe_check(JNIEnv *e,jclass,jobject context,jstring path){
 const char *p=e->GetStringUTFChars(path,nullptr);void *lib=dlopen(p,RTLD_NOW|RTLD_LOCAL);e->ReleaseStringUTFChars(path,p);if(!lib)return -1;
 auto init=reinterpret_cast<LyraPluginInitFn>(dlsym(lib,"lyra_plugin_init"));if(!init){dlclose(lib);return -2;}
 JavaVM *vm=nullptr;e->GetJavaVM(&vm);ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
 LyraHostV3 h{};h.size=sizeof(h);h.abi=LYRA_PLUGIN_ABI;h.java_vm=vm;h.android_context=context;h.query_interface=query;
 LyraPluginV3 out{};int status=init(&h,&out);if(!status)status=out.init(&h);
 if(!status){for(int i=0;i<3;++i){int fd,events;void *data;ALooper_pollOnce(300,&fd,&events,&data);}out.shutdown(&h);}
 if(e->ExceptionCheck()){e->ExceptionDescribe();e->ExceptionClear();status=-3;}
 dlclose(lib);return status;
}
