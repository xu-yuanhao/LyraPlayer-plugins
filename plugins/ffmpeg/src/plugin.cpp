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
#ifndef LYRA_PLUGIN_ID
#define LYRA_PLUGIN_ID "official.ffmpeg"
#endif
#include <jni.h>
// Called only when the engine needs a new stream. Use the Activity
// supplied by the host and keep JNI references local to this query.
static int screen_active_query(void *user) {
    auto *host = static_cast<const LyraHostV3 *>(user);
    if (!host->java_vm || !host->android_context) return 1;
    auto *vm = static_cast<JavaVM *>(host->java_vm);
    JNIEnv *env = nullptr;
    const jint status = vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    const bool attach = status == JNI_EDETACHED;
    if (attach && vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return 1;
    if (!env) return 1;
    int active = 1;
    if (env->PushLocalFrame(8) == JNI_OK) {
        auto context = static_cast<jobject>(host->android_context);
        auto type = env->GetObjectClass(context);
        auto service = type ? env->GetMethodID(type, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;") : nullptr;
        auto name = service ? env->NewStringUTF("power") : nullptr;
        auto power = name ? env->CallObjectMethod(context, service, name) : nullptr;
        auto power_type = power ? env->GetObjectClass(power) : nullptr;
        auto is_interactive = power_type ? env->GetMethodID(power_type, "isInteractive", "()Z") : nullptr;
        if (is_interactive) active = env->CallBooleanMethod(power, is_interactive) ? 1 : 0;
        env->PopLocalFrame(nullptr);
    }
    if (env->ExceptionCheck()) { env->ExceptionClear(); active = 1; }
    if (attach) vm->DetachCurrentThread();
    return active;
}
static OwnedPlaybackBackend backend;
static int init(const LyraHostV3 *host) {
    backend = make_engine_backend();
    if (backend.set_screen_query)
        backend.set_screen_query(backend.api.user, screen_active_query, const_cast<LyraHostV3 *>(host));
    lyra_log(LyraLogInfo,"Lyra","ffmpeg playback provider ready version=%s",LYRA_PLUGIN_VERSION);
    return backend.api.user ? 0 : -1;
}
static void shutdown(const LyraHostV3*) {
    if (backend.destroy) backend.destroy(backend.api.user);
    backend = {};
}
static const void* query(const LyraHostV3*, uint32_t id, uint32_t version, uint32_t size) {
    if (version != LYRA_INTERFACE_VERSION) return nullptr;
    if (id == LYRA_INTERFACE_AUDIO_TRANSITION && size == sizeof(LyraAudioTransitionV3)) return backend.transition;
    return id == LYRA_INTERFACE_PLAYBACK_PROVIDER && size == sizeof(backend.api) ? &backend.api : nullptr;
}
extern "C" __attribute__((visibility("default"))) int lyra_plugin_init(const LyraHostV3* host,LyraPluginV3* out){
 if(!host||host->size!=sizeof(*host)||host->abi!=LYRA_PLUGIN_ABI||!out)return -1;*out={};out->size=sizeof(*out);out->abi=LYRA_PLUGIN_ABI;out->id=LYRA_PLUGIN_ID;out->kind="playback";out->config_tables="audio_transition";out->version=LYRA_PLUGIN_VERSION;out->init=init;out->shutdown=shutdown;out->query_interface=query;return 0;
}
