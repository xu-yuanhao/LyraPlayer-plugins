#include "plugin/lyra_plugin.h"
#include "playback/provider.h"
#include <cstring>
#ifndef LYRA_PLUGIN_VERSION
#error "Build must provide the catalog plugin version"
#endif
static OwnedPlaybackBackend backend;
static int init(const LyraHostV3*){backend=make_engine_backend();return backend.api.user?0:-1;}
static void shutdown(const LyraHostV3*){if(backend.destroy)backend.destroy(backend.api.user);backend={};}
static const void* query(const LyraHostV3*,uint32_t id,uint32_t version,uint32_t size){return id==LYRA_INTERFACE_PLAYBACK_PROVIDER&&version==LYRA_INTERFACE_VERSION&&size==sizeof(backend.api)?&backend.api:nullptr;}
extern "C" __attribute__((visibility("default"))) int lyra_plugin_init(const LyraHostV3* host,LyraPluginV3* out){
 if(!host||host->abi!=LYRA_PLUGIN_ABI||!out)return -1;*out={};out->size=sizeof(*out);out->abi=LYRA_PLUGIN_ABI;out->id="official.ffmpeg";out->kind="playback";out->version=LYRA_PLUGIN_VERSION;out->init=init;out->shutdown=shutdown;out->query_interface=query;return 0;
}
