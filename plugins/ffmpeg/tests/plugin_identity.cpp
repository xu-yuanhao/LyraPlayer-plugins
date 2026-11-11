#include "plugin/lyra_plugin.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
int main(int argc,char **argv){
 if(argc!=3)return 2;void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
 if(!lib){puts(dlerror());return 1;}
 auto entry=reinterpret_cast<int(*)(const LyraHostV3*,LyraPluginV3*)>(dlsym(lib,"lyra_plugin_init"));
 LyraHostV3 host{};host.size=sizeof(host);host.abi=LYRA_PLUGIN_ABI;LyraPluginV3 plugin{};
 if(!entry || entry(&host,&plugin) || !plugin.id || std::strcmp(plugin.id,argv[2]) || std::strcmp(plugin.version,"1.4.6"))return 1;
 printf("PASS actual plugin identity %s version %s\n",plugin.id,plugin.version);dlclose(lib);
}
