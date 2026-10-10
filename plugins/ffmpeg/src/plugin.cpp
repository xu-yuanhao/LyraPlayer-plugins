#include "plugin/lyra_plugin.h"
#include "playback/provider.h"
#include <cstring>
static OwnedPlaybackBackend backend;
static int init(const LyraHostV2*){backend=make_engine_backend();return backend.api.user?0:-1;}
static void shutdown(const LyraHostV2*){if(backend.destroy)backend.destroy(backend.api.user);backend={};}
static const void* query(const LyraHostV2*,uint32_t id,uint32_t version,uint32_t size){return id==LYRA_INTERFACE_PLAYBACK_PROVIDER&&version==2&&size<=sizeof(backend.api)?&backend.api:nullptr;}
extern "C" __attribute__((visibility("default"))) int lyra_plugin_init(const LyraHostV2* host,LyraPluginV2* out){
 if(!host||host->abi!=2||!out)return -1;*out={};out->size=sizeof(*out);out->abi=2;out->id="official.ffmpeg";out->kind="playback";out->version="1.0.0";out->init=init;out->shutdown=shutdown;out->query_interface=query;return 0;
}
