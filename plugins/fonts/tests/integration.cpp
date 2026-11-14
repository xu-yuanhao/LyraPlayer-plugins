#include "runtime/settings_page.h"
#include "runtime/settings_extension.h"
#include "config/store.h"
#include "render/scene.h"
#include "../src/plugin.cpp"
#include <cassert>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
namespace {std::string storage;int picked=-1;bool picker_allowed=true;lyra::ConfigStore config;SettingsPage page;LyraPluginV3 plugin{};int64_t clock_ns=1;bool installed=true;}
bool font_platform_ready(const LyraHostV3*){return true;}
std::string font_storage(const LyraHostV3*){return storage;}
int font_pick(const LyraHostV3*,const char*,const char*){return picker_allowed?0:-1;}
int font_pick_result(const LyraHostV3*){int fd=picked;picked=-1;return fd;}
int main(int argc,char**argv){
 assert(argc==3);char root[]="/tmp/lyra-font-plugin-XXXXXX";assert(mkdtemp(root));storage=root;
 assert(mkdir((storage+"/lyra").c_str(),0700)==0);assert(mkdir((storage+"/lyra/config").c_str(),0700)==0);
 config.set_assets(read_text_file(argv[1]),read_text_file(argv[2]));config.set_roots("",storage);lyra::UiConfig ui;lyra::TextConfig copy;std::string error;assert(config.load(false,&ui,&copy,&error));config.commit(ui,copy);
 ThemeController theme;theme.configure(ui,0);theme.update(ui,{.5f,.2f,.1f,1},0);TextLayoutCache glyphs;ImageResources images;
 LyraServiceContextV3 context{};context.size=sizeof(context);context.version=LYRA_INTERFACE_VERSION;
 context.count=[](void*){return 1u;};context.at=[](void*,uint32_t){return uint64_t(1);};
 context.interface=[](void*,uint64_t handle,uint32_t selected,uint32_t id,uint32_t version,uint32_t size)->const void*{if(!installed || handle!=1 || selected!=LYRA_SERVICE_SELECTED)return nullptr;return plugin.query_interface?plugin.query_interface(nullptr,id,version,size):nullptr;};
 PageFrame frame;frame.config=&config.settings();frame.text=&config.text();frame.palette=&theme.palette();frame.glyphs=&glyphs;frame.images=&images;frame.width=400;frame.height=800;frame.extensions=&context;page.bind(frame);
 LyraHostV3 host{};host.size=sizeof(host);host.abi=LYRA_PLUGIN_ABI;
 host.now_ns=[](void*){return clock_ns;};host.request_frame=[](void*){};
 host.query_interface=[](void*,uint32_t id,uint32_t version,uint32_t size)->const void*{if(version!=LYRA_INTERFACE_VERSION)return nullptr;
  if(id==LYRA_INTERFACE_CONFIG && size==sizeof(LyraConfigInterfaceV3))return config.interface();
  if(id==LYRA_INTERFACE_CONFIG_SCHEMA && size==sizeof(LyraConfigSchemaV3))return config.schema_interface();
  if(id==LYRA_INTERFACE_SETTINGS_PAGE && size==sizeof(LyraSettingsPageV3))return page.extension_interface();return nullptr;};
 assert(lyra_plugin_init(&host,&plugin)==0 && plugin.init(&host)==0);
 auto update=[&]{if(config.take_changed()){assert(config.load(false,&ui,&copy,&error));config.commit(ui,copy);}clock_ns+=250000000;plugin.on_frame(&host,clock_ns);page.sync_extension();};
 SettingsExtension ext;ext.context=&context;auto entries=ext.entries();assert(entries.size()==1 && entries[0].title==strings::fonts_title);
 page.navigate(SettingsPage::MainPage);int entry=-1;for(int i=0;i<page.view().settings_count_;++i)if(page.view().settings_rows_[i].extension)entry=i;assert(entry>=0);
 page.open_extension(1);assert(page.settings_heading()==strings::fonts_title);assert(page.view().settings_count_==17);
 UiLayout layout;layout.settings={0,0,400,800};layout.drawer_w=400;layout.row_h=64;
 auto row=[&](const char *title){for(int i=0;i<page.view().settings_count_;++i)if(page.view().settings_rows_[i].title==title)return i;assert(false);return -1;};
 auto tap=[&](int index,const char *value=nullptr,bool secondary=false){SettingsIntent intent;intent.kind=SettingsIntent::Kind::ExtensionAction;intent.index=index;intent.secondary=secondary;if(value)intent.url=value;page.extension_action(intent);update();};
 // Same six size/bold controls, all locks and persisted values.
 tap(row(strings::fonts_follow_system));assert(config.settings().fonts.follow_system && page.view().settings_rows_[1].disabled);tap(0);assert(!config.settings().fonts.follow_system);
 auto parameter=row(strings::fonts_follow_parameters);tap(parameter);assert(config.settings().fonts.follow_parameters);for(int i=parameter+1;i<page.view().settings_count_-1;++i)assert(page.view().settings_rows_[i].disabled);tap(parameter);
 auto general=std::string(strings::font_styles_row_title_sp)+strings::fonts_size;tap(row(general.c_str()),"24");assert(config.settings().row_title_sp==24);
 tap(row((std::string(strings::font_styles_row_title_sp)+strings::fonts_bold).c_str()));assert(config.settings().fonts.bold[lyra::fonts::FRowTitleSp]);
 // slider gesture uses the host's shared rendering/input, not a plugin UI clone.
 page.sync_settings_range(layout);int size_row=row(general.c_str());auto metrics=page.settings_metrics(layout);page.scroll_move(page.row_top(size_row,metrics)-metrics.content_top);float y=page.row_top(size_row,metrics)+metrics.row_h*.5f;
 float x=350;assert(page.control_touch(layout,{sizeof(LyraInput),LYRA_INPUT_DOWN,x,y,0}));assert(page.control_touch(layout,{sizeof(LyraInput),LYRA_INPUT_UP,x,y,1}));auto changed=page.intent(layout,x,y,1);assert(changed.kind==SettingsIntent::Kind::ExtensionAction);page.extension_action(changed);update();assert(config.settings().row_title_sp>=config.settings().fonts.minimum[lyra::fonts::FRowTitleSp] && config.settings().row_title_sp<=config.settings().fonts.maximum[lyra::fonts::FRowTitleSp] && config.settings().row_title_sp!=24);
 // Import a real OpenType metadata fixture, multiple weights grouped as one family.
 std::vector<unsigned char> data(140);auto be16=[&](size_t at,unsigned n){data[at]=n>>8;data[at+1]=n;};auto be32=[&](size_t at,unsigned n){be16(at,n>>16);be16(at+2,n);};
 be32(0,0x10000);be16(4,4);const char *tags[]={"name","head","cmap","OS/2"};unsigned offsets[]={76,112,124,128},lengths[]={36,12,4,8};for(unsigned i=0;i<4;++i){auto at=12+i*16;memcpy(data.data()+at,tags[i],4);be32(at+8,offsets[i]);be32(at+12,lengths[i]);}be16(78,1);be16(80,18);be16(82,3);be16(84,1);be16(86,0x409);be16(88,16);be16(90,16);for(unsigned i=0;i<8;++i)data[94+i*2+1]="PingFang"[i];be16(132,400);
 auto source=storage+"/source.ttf";auto save=[&]{int fd=open(source.c_str(),O_RDWR|O_CREAT|O_TRUNC,0600);assert(fd>=0 && write(fd,data.data(),data.size())==ssize_t(data.size()));assert(lseek(fd,0,SEEK_SET)==0);return fd;};
 auto import=[&]{tap(row(strings::fonts_install));picked=save();update();for(int i=0;i<2000;++i){update();if(instance->notice==strings::fonts_installed)break;std::this_thread::sleep_for(std::chrono::milliseconds(1));}update();};
 picker_allowed=false;tap(row(strings::fonts_install));assert(instance->notice==strings::fonts_invalid && !instance->catalog.picking);picker_allowed=true;
 import();assert(instance->catalog.view().choices.size()==1);
 be16(132,700);import();assert(instance->catalog.view().choices.size()==1 && instance->catalog.view().choices[0].spec.find("700=")!=std::string::npos);assert(instance->catalog.view().choices[0].user);auto id=instance->catalog.view().choices[0].id;
 tap(row(strings::fonts_installed_title));assert(page.settings_heading()==strings::fonts_installed_title);tap(0);assert(config.settings().fonts.family==id);char descriptor[4096];assert(plugin.supply(&host,0,descriptor,sizeof(descriptor))>0 && std::string(descriptor).find("lyra:")==0);
 // Reset confirmation cancellation/back never modifies persistent data.
 tap(row(strings::fonts_reset));Scene scene;page.paint_settings(scene,layout);assert(scene.popup.on && scene.popup.glass);page.back(true);update();assert(config.settings().fonts.family==id);
 assert(config.interface()->write(config.interface()->user,"system.screen_off_throttle","true")==0);update();
 tap(row(strings::fonts_reset));assert(instance->confirm(true));update();assert(config.settings().fonts.family=="auto" && config.settings().row_title_sp==16 && !config.settings().fonts.modified && config.settings().screen_off_throttle);
 tap(row(strings::fonts_installed_title));tap(0,nullptr,true);assert(!instance->prompt.empty());assert(instance->confirm(true));update();assert(instance->catalog.view().choices.empty());
 // Invalid requests have no effects; optional unknown tags permitted, required rejected.
 auto *api=static_cast<const LyraSettingsPageV3*>(plugin.query_interface(&host,LYRA_INTERFACE_SETTINGS_PAGE,LYRA_INTERFACE_VERSION,sizeof(LyraSettingsPageV3)));assert(api);assert(!plugin.query_interface(&host,LYRA_INTERFACE_SETTINGS_PAGE,LYRA_INTERFACE_VERSION,sizeof(LyraSettingsPageV3)-1));
 LyraMessageV3 msg{sizeof(msg)-1,0,LYRA_SETTINGS_ACTIVATE,nullptr};LyraReplyV3 reply{sizeof(reply),0,0,0,nullptr};assert(api->request(api->user,&msg,&reply)==LYRA_MESSAGE_INVALID);msg.size=sizeof(msg);msg.operation=LYRA_SETTINGS_INFO;assert(api->request(api->user,&msg,&reply)==LYRA_MESSAGE_CAPACITY);
 LyraFieldV3 invalid{999,LYRA_FIELD_UTF8,LYRA_FIELD_REQUIRED,1,""};msg.field_count=1;msg.fields=&invalid;assert(api->request(api->user,&msg,&reply)==LYRA_MESSAGE_UNSUPPORTED);
 // Legacy installed-font manager remains available, with exact size/type guards.
 auto *manager=static_cast<const LyraFontManagerV3*>(plugin.query_interface(&host,LYRA_INTERFACE_FONT_MANAGER,LYRA_INTERFACE_VERSION,sizeof(LyraFontManagerV3)));assert(manager);
 LyraFieldV3 path{LYRA_FONT_CHOICE,LYRA_FIELD_UTF8,LYRA_FIELD_REQUIRED,14,"../source.ttf"};LyraMessageV3 removal{sizeof(removal),1,LYRA_FONT_REMOVE,&path};reply={sizeof(reply),0,0,0,nullptr};assert(manager->request(manager->user,&removal,&reply)==LYRA_MESSAGE_INVALID);
 // Revocation during the settings page returns safely to main, no retained interface.
 installed=false;page.sync_extension();assert(page.view().settings_page_==SettingsPage::MainPage);plugin.shutdown(&host);unlink(source.c_str());rmdir((storage+"/lyra/fonts").c_str());rmdir((storage+"/lyra").c_str());
 puts("font plugin: settings/locks/slider/import/select/reset/delete/ABI/revocation passed");
}
