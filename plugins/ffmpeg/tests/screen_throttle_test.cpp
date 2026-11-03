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
    for(const char *text:{"","other=true\n","screen_off_throttle=false\n","screen_off_throttle=1\n","screen_off_throttle=\"true\"\n","screen_off_throttle=truex\n","screen_off_throttlex=true\n"})assert(!screen_throttle_enabled(text));
    assert(screen_throttle_enabled("haptics=true\nscreen_off_throttle=true\nnext=0\n"));
    assert(screen_throttle_enabled(" \tscreen_off_throttle \t= true \r\n"));
    std::string config="screen_off_throttle=false\n";
    LyraHostV3 host{};host.size=sizeof(host);host.abi=LYRA_PLUGIN_ABI;host.user=&config;
    host.config_text=[](void *u,const char *table,char *out,int cap){assert(std::string(table)=="system");return std::snprintf(out,cap,"%s",static_cast<std::string*>(u)->c_str());};
    LyraPluginV3 plugin{};assert(lyra_plugin_init(&host,&plugin)==0);
    assert(std::string(plugin.config_tables)=="audio_transition,system");
    assert(plugin.init(&host)==0);assert(!screen_throttle && poll_screen(poll_user)==1);
    auto *api=static_cast<const LyraAudioTransitionV3*>(plugin.query_interface(&host,LYRA_INTERFACE_AUDIO_TRANSITION,LYRA_INTERFACE_VERSION,sizeof(LyraAudioTransitionV3)));
    assert(api && api!=control.interface());
    config="screen_off_throttle=true\n";
    assert(configure_transition(api,{true,7000,4000})==0);assert(screen_throttle);
    // Missing Android context safely retains the foreground stream.
    assert(poll_screen(poll_user)==1);
    JNINativeInterface jni{};JNIInvokeInterface invoke{};
    JNIEnv env{&jni};JavaVM vm{&invoke};test_env=&env;
    invoke.GetEnv=[](JavaVM*,void **out,jint){*out=test_env;return jint(JNI_OK);};
    jni.CallBooleanMethodV=[](JNIEnv*,jobject,jmethodID,va_list){++screen_reads;return jboolean(interactive);};
    jni.ExceptionCheck=[](JNIEnv*){return jboolean(JNI_FALSE);};
    g_vm=&vm;g_power=reinterpret_cast<jobject>(1);g_is_interactive=reinterpret_cast<jmethodID>(1);
    for(int i=0;i<6;++i){interactive=false;assert(poll_screen(poll_user)==0);interactive=true;assert(poll_screen(poll_user)==1);}
    assert(screen_reads==12);
    assert(automatic_transition_ms(api)==7000);
    config="screen_off_throttle=false\n";
    assert(configure_transition(api,{true,6000,3000})==0);assert(!screen_throttle && poll_screen(poll_user)==1);
    interactive=false;
    for(int i=0;i<12;++i)assert(poll_screen(poll_user)==1);
    assert(screen_reads==12); // disabled policy never queries Android.
    g_vm=nullptr;g_power=nullptr;g_is_interactive=nullptr;
    assert(automatic_transition_ms(api)==6000);
    config="";assert(configure_transition(api,{false,6000,3000})==0);assert(!screen_throttle);
    LyraPlaybackCommandV3 command{};command.size=sizeof(command);
    auto *provider=static_cast<const LyraPlaybackProviderV3*>(plugin.query_interface(&host,LYRA_INTERFACE_PLAYBACK_PROVIDER,LYRA_INTERFACE_VERSION,sizeof(LyraPlaybackProviderV3)));
    assert(provider && provider->command(provider->user,&command)==1);
    assert(!plugin.query_interface(&host,LYRA_INTERFACE_AUDIO_TRANSITION,LYRA_INTERFACE_VERSION,sizeof(LyraAudioTransitionV3)-1));
    plugin.shutdown(&host);assert(!screen_throttle && !native_transition);
    host.config_text=nullptr;assert(plugin.init(&host)==0);assert(!screen_throttle && poll_screen(poll_user)==1);plugin.shutdown(&host);
    std::puts("Screen throttle defaults, live config updates, old hosts, ABI and transition forwarding passed");
}
