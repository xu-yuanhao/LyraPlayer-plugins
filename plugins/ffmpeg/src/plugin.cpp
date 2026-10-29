#include "plugin/lyra_plugin.h"
#include "playback/provider.h"
#include "plugin/transition_contracts.h"
#include "platform/log.h"
#include <cstring>
#include <mutex>
#include "playback/transition_control.h"
#ifndef LYRA_PLUGIN_VERSION
#error "Build must provide the catalog plugin version"
#endif
#include <jni.h>
// The host hands us its JavaVM and a Context so we can answer "is the screen
// interactive?" ourselves; the playback engine polls this once a second to size
// the output buffer (screen-off = large/low-power, screen-on = small/low-latency).
static JavaVM *g_vm = nullptr;
static jobject g_context = nullptr; // global ref
static jobject g_power = nullptr;   // global ref to PowerManager
static jmethodID g_is_interactive = nullptr;
// The host fills java_vm/android_context into this same struct after init (via
// set_platform), so read them lazily at first poll rather than only at init.
static const LyraHostV3 *platform_host = nullptr;
static void screen_jni_init(void *vm, void *context) {
    if (!vm || !context) return;
    g_vm = static_cast<JavaVM *>(vm);
    JNIEnv *env = nullptr;
    bool attached = false;
    if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    } else if (!env) return;
    g_context = env->NewGlobalRef(static_cast<jobject>(context));
    if (g_context) {
        jclass ctx = env->GetObjectClass(g_context);
        jmethodID gss = env->GetMethodID(ctx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
        if (gss) {
            jstring name = env->NewStringUTF("power");
            jobject power = env->CallObjectMethod(g_context, gss, name);
            if (power) {
                g_power = env->NewGlobalRef(power);
                jclass pm = env->GetObjectClass(power);
                g_is_interactive = env->GetMethodID(pm, "isInteractive", "()Z");
            }
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) g_vm->DetachCurrentThread();
}
static int screen_active_query(void *) {
    if (!g_power) {
        if (platform_host) screen_jni_init(platform_host->java_vm, platform_host->android_context);
        if (!g_power) return 1;
    }
    JNIEnv *env = nullptr;
    bool attached = false;
    jint st = g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (st == JNI_EDETACHED) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return 1;
        attached = true;
    } else if (st != JNI_OK || !env) {
        return 1;
    }
    int on = env->CallBooleanMethod(g_power, g_is_interactive) ? 1 : 0;
    if (env->ExceptionCheck()) { env->ExceptionClear(); on = 1; }
    if (attached) g_vm->DetachCurrentThread();
    return on;
}
static OwnedPlaybackBackend backend;
static int (*native_command)(void*,const LyraPlaybackCommandV3*)=nullptr;
static int command(void *user,const LyraPlaybackCommandV3 *c){return native_command?native_command(user,c):-1;}
static int init(const LyraHostV3 *host){backend=make_engine_backend();platform_host=host;native_command=backend.api.command;backend.api.command=command;
 if(backend.set_screen_query){if(host->java_vm&&host->android_context)screen_jni_init(host->java_vm,host->android_context);backend.set_screen_query(backend.api.user,screen_active_query,nullptr);}
 lyra_log(LyraLogInfo,"Lyra","ffmpeg playback provider ready version=%s",LYRA_PLUGIN_VERSION);return backend.api.user?0:-1;}
static void shutdown(const LyraHostV3*){if(backend.destroy)backend.destroy(backend.api.user);backend={};platform_host=nullptr;native_command=nullptr;}
static const void* query(const LyraHostV3*,uint32_t id,uint32_t version,uint32_t size){if(id==LYRA_INTERFACE_AUDIO_TRANSITION&&version==LYRA_INTERFACE_VERSION&&size==sizeof(LyraAudioTransitionV3))return backend.transition;return id==LYRA_INTERFACE_PLAYBACK_PROVIDER&&version==LYRA_INTERFACE_VERSION&&size==sizeof(backend.api)?&backend.api:nullptr;}
extern "C" __attribute__((visibility("default"))) int lyra_plugin_init(const LyraHostV3* host,LyraPluginV3* out){
 if(!host||host->size!=sizeof(*host)||host->abi!=LYRA_PLUGIN_ABI||!out)return -1;*out={};out->size=sizeof(*out);out->abi=LYRA_PLUGIN_ABI;out->id="official.ffmpeg";out->kind="playback";out->config_tables="audio_transition";out->version=LYRA_PLUGIN_VERSION;out->init=init;out->shutdown=shutdown;out->query_interface=query;return 0;
}
