#include "../src/plugin.cpp"
#include "playback/transition_control.h"
#include <cassert>
#include <cstdio>
#include <string>
static AudioTransitionControl control;
static int (*poll_screen)(void*)=nullptr;
static void *poll_user=nullptr;
static int commands=0, screen_reads=0;
static bool interactive=false;
static JNIEnv *test_env=nullptr;
OwnedPlaybackBackend make_engine_backend() {
    OwnedPlaybackBackend out;out.api.size=sizeof(out.api);out.api.version=LYRA_INTERFACE_VERSION;out.api.user=&control;
    out.api.command=[](void*,const LyraPlaybackCommandV3*){return ++commands;};
    out.transition=control.interface();
    out.set_screen_query=[](void*,int(*fn)(void*),void *u){poll_screen=fn;poll_user=u;};
    return out;
}
int lyra_log(int,const char*,const char*,...) {return 0;}
int main() {
    LyraHostV3 host{};host.size=sizeof(host);host.abi=LYRA_PLUGIN_ABI;
    LyraPluginV3 plugin{};assert(lyra_plugin_init(&host,&plugin)==0);
    assert(std::string(plugin.config_tables)=="audio_transition");
    assert(plugin.init(&host)==0 && poll_screen(poll_user)==1);
    auto *api=static_cast<const LyraAudioTransitionV3*>(plugin.query_interface(&host,LYRA_INTERFACE_AUDIO_TRANSITION,LYRA_INTERFACE_VERSION,sizeof(LyraAudioTransitionV3)));
    assert(api==control.interface());
    assert(configure_transition(api,{true,7000,4000})==0);
    assert(automatic_transition_ms(api)==7000);
    JNINativeInterface jni{};JNIInvokeInterface invoke{};
    JNIEnv env{&jni};JavaVM vm{&invoke};test_env=&env;
    host.java_vm=&vm;host.android_context=reinterpret_cast<jobject>(1);
    invoke.GetEnv=[](JavaVM*,void **out,jint){*out=test_env;return jint(JNI_OK);};
    jni.PushLocalFrame=[](JNIEnv*,jint){return jint(JNI_OK);};
    jni.PopLocalFrame=[](JNIEnv*,jobject){return jobject(nullptr);};
    jni.GetObjectClass=[](JNIEnv*,jobject){return reinterpret_cast<jclass>(1);};
    jni.GetMethodID=[](JNIEnv*,jclass,const char*,const char*){return reinterpret_cast<jmethodID>(1);};
    jni.NewStringUTF=[](JNIEnv*,const char*){return reinterpret_cast<jstring>(1);};
    jni.CallObjectMethodV=[](JNIEnv*,jobject,jmethodID,va_list){return reinterpret_cast<jobject>(1);};
    jni.CallBooleanMethodV=[](JNIEnv*,jobject,jmethodID,va_list){++screen_reads;return jboolean(interactive);};
    jni.ExceptionCheck=[](JNIEnv*){return jboolean(JNI_FALSE);};
    interactive=false;assert(poll_screen(poll_user)==0);
    interactive=true;assert(poll_screen(poll_user)==1);
    assert(screen_reads==2);
    LyraPlaybackCommandV3 command{};command.size=sizeof(command);
    auto *provider=static_cast<const LyraPlaybackProviderV3*>(plugin.query_interface(&host,LYRA_INTERFACE_PLAYBACK_PROVIDER,LYRA_INTERFACE_VERSION,sizeof(LyraPlaybackProviderV3)));
    assert(provider && provider->command(provider->user,&command)==1);
    assert(!plugin.query_interface(&host,LYRA_INTERFACE_AUDIO_TRANSITION,LYRA_INTERFACE_VERSION,sizeof(LyraAudioTransitionV3)-1));
    plugin.shutdown(&host);
    std::puts("Screen query, ABI and transition forwarding passed");
}
