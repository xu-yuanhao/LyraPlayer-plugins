#include "plugin/lyra_plugin.h"
#include "playback/provider.h"
#include "plugin/transition_contracts.h"
#include "platform/log.h"
#include <cstring>
#include <mutex>
#include "stems_dependency.h"
#include "playback/transition_control.h"
#ifndef LYRA_PLUGIN_VERSION
#error "Build must provide the catalog plugin version"
#endif
static OwnedPlaybackBackend backend;
static StemDependency stems;
static const LyraHostV3 *platform_host=nullptr;static std::string module_root;static std::mutex dependency_mutex;
static int (*native_command)(void*,const LyraPlaybackCommandV3*)=nullptr;
static void ensure_stems(){std::lock_guard<std::mutex> lock(dependency_mutex);if(stems.api || !platform_host || !platform_host->android_context)return;
 if(stems.load(platform_host,module_root)){bind_transition_stems(backend.transition,stems.api);lyra_log(LyraLogInfo,"Lyra","neural stem endpoint loaded");}}
static int command(void *user,const LyraPlaybackCommandV3 *c){if(c && c->size==sizeof(*c) && c->kind==LYRA_PLAYBACK_OPEN)ensure_stems();return native_command?native_command(user,c):-1;}
static int init(const LyraHostV3 *host){backend=make_engine_backend();platform_host=host;native_command=backend.api.command;backend.api.command=command;Dl_info info{};
 if(dladdr(reinterpret_cast<void*>(&init),&info) && info.dli_fname){module_root=info.dli_fname;module_root.resize(module_root.find_last_of('/'));}ensure_stems();
 lyra_log(LyraLogInfo,"Lyra","ffmpeg transition provider ready version=%s",LYRA_PLUGIN_VERSION);return backend.api.user?0:-1;}
static void shutdown(const LyraHostV3*){if(backend.destroy)backend.destroy(backend.api.user);backend={};stems.close();platform_host=nullptr;native_command=nullptr;module_root.clear();}
static const void* query(const LyraHostV3*,uint32_t id,uint32_t version,uint32_t size){ensure_stems();if(id==LYRA_INTERFACE_STEM_ANALYSIS && version==LYRA_INTERFACE_VERSION && size==sizeof(LyraStemAnalysisV3))return stems.api;if(id==LYRA_INTERFACE_AUDIO_TRANSITION&&version==LYRA_INTERFACE_VERSION&&size==sizeof(LyraAudioTransitionV3))return backend.transition;return id==LYRA_INTERFACE_PLAYBACK_PROVIDER&&version==LYRA_INTERFACE_VERSION&&size==sizeof(backend.api)?&backend.api:nullptr;}
extern "C" __attribute__((visibility("default"))) int lyra_plugin_init(const LyraHostV3* host,LyraPluginV3* out){
 if(!host||host->size!=sizeof(*host)||host->abi!=LYRA_PLUGIN_ABI||!out)return -1;*out={};out->size=sizeof(*out);out->abi=LYRA_PLUGIN_ABI;out->id="official.ffmpeg";out->kind="playback";out->config_tables="audio_transition";out->version=LYRA_PLUGIN_VERSION;out->init=init;out->shutdown=shutdown;out->query_interface=query;return 0;
}
