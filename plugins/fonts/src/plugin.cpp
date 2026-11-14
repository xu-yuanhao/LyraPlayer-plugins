#include "plugin/lyra_plugin.h"
#include "plugin/settings_contracts.h"
#include "plugin/font_contracts.h"
#include "font_catalog.h"
#include "platform.h"
#include "strings.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <memory>
namespace {
struct Item {std::string title,detail,button,key,id;int64_t flags=0;double value=0,minimum=0,maximum=0,step=0;int action=0;};
enum {Follow=1,Choose,Import,Installed,Parameters,Size,Bold,Reset,Select,Remove};
struct Fonts {
 const LyraHostV3 *host=nullptr;
 const LyraConfigInterfaceV3 *config=nullptr;
 const LyraConfigSchemaV3 *schema=nullptr;
 lyra::ManagedFontCatalog catalog;
 lyra::custom_fonts::Settings settings;
 std::vector<Item> rows;
 int64_t page=0,revision=1,count=0,reply_integer=0;
 std::string title,prompt,notice,delete_id,stamp,root;
 int64_t polled_ns=0;
 bool reset=false,ready=false;
 LyraSettingsPageV3 api{};LyraFontManagerV3 manager{};
 std::string scalar(const std::string &key,int field=0)const{
  if(!schema)return {};int n=schema->text(schema->user,key.c_str(),field,nullptr,0);if(n<=0 || n>65536)return {};std::string s(size_t(n),0);if(schema->text(schema->user,key.c_str(),field,s.data(),n)!=n)return {};s.resize(size_t(n)-1);return s;
 }
 static std::string unquote(std::string s){if(s.size()<2 || s.front()!='"' || s.back()!='"')return s;std::string out;for(size_t i=1;i+1<s.size();++i){char c=s[i];if(c=='\\' && i+2<s.size()){c=s[++i];if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';}out+=c;}return out;}
 static std::string quote(const std::string &s){std::string out="\"";for(char c:s){if(c=='"' || c=='\\')out+='\\';if(c=='\n')out+="\\n";else if(c=='\r')out+="\\r";else if(c=='\t')out+="\\t";else out+=c;}return out+'"';}
 double number(const char *key,int field=0)const{return std::strtod(scalar(key,field).c_str(),nullptr);}
 bool write(const std::string &key,const std::string &value){bool okay=config && config->write(config->user,key.c_str(),value.c_str())==0;if(!okay)notice="保存设置失败";if(host && host->request_frame)host->request_frame(host->user);stamp.clear();++revision;return okay;}
 bool toggle(const char *key){return write(key,scalar(key)=="true"?"false":"true");}
 void refresh(){
  std::string next;auto read=[&](const char *k){auto s=scalar(k);next+=s;next+='\n';return s;};
  settings.family=unquote(read("typography.family"));settings.follow_system=read("typography.follow_system")=="true";settings.follow_parameters=read("typography.follow_parameters")=="true";
  settings.minimum_ratio=std::strtod(read("typography.minimum_ratio").c_str(),nullptr);settings.maximum_ratio=std::strtod(read("typography.maximum_ratio").c_str(),nullptr);
  settings.minimum_size_sp=number("typography.minimum_size_sp");settings.maximum_size_sp=number("typography.maximum_size_sp");settings.import_max_bytes=number("typography.import_max_bytes");settings.import_timeout_ms=number("typography.import_timeout_ms");settings.import_limit=number("typography.import_limit");settings.recommended_limit=number("typography.recommended_limit");settings.import_mime_types=unquote(scalar("typography.import_mime_types"));settings.preferred_families=unquote(scalar("typography.preferred_families"));
  for(auto role:lyra::custom_fonts::controls){auto i=size_t(role);auto v=read(lyra::custom_fonts::size_keys[i]);auto b=read(lyra::custom_fonts::bold_keys[i]);settings.sizes[i]=std::strtod(v.c_str(),nullptr);settings.bold[i]=b=="true";
   auto base=number(lyra::custom_fonts::size_keys[i],1);settings.minimum[i]=std::max(settings.minimum_size_sp,std::ceil(base*settings.minimum_ratio));settings.maximum[i]=std::min(settings.maximum_size_sp,std::floor(base*settings.maximum_ratio));settings.sizes[i]=std::clamp(settings.sizes[i],settings.minimum[i],settings.maximum[i]);
  }
  if(next!=stamp){stamp=std::move(next);++revision;settings.modified=false;
   for(auto key:{"typography.family","typography.follow_system","typography.follow_parameters"})settings.modified|=scalar(key)!=scalar(key,1);
   for(size_t i=0;i<lyra::custom_fonts::count;++i){settings.modified|=scalar(lyra::custom_fonts::size_keys[i])!=scalar(lyra::custom_fonts::size_keys[i],1);settings.modified|=scalar(lyra::custom_fonts::bold_keys[i])!=scalar(lyra::custom_fonts::bold_keys[i],1);}catalog.preferences(settings);catalog.configure(int(number("player.font_weight_regular")),int(number("player.font_weight_light")),int(number("player.font_weight_bold")),int(number("player.font_weight_heavy")));build();}
 }
 static bool mutable_key(const std::string &k){return k=="typography.family" || k=="typography.follow_system" || k=="typography.follow_parameters" || k.rfind("typography.sizes.",0)==0 || k.rfind("typography.bold.",0)==0;}
 void build(){
  using namespace strings;rows.clear();
  auto add=[&](const char *label,int action,int64_t flags=0,const std::string &detail={}) -> Item&{rows.push_back({label,detail});auto &r=rows.back();r.action=action;r.flags=flags;return r;};
  auto locked=settings.follow_system?LYRA_SETTINGS_DISABLED:0;auto controls=settings.follow_parameters?LYRA_SETTINGS_DISABLED:0;
  if(page==0){
   title=fonts_title;add(fonts_follow_system,Follow,LYRA_SETTINGS_TOGGLE|(settings.follow_system?LYRA_SETTINGS_CHECKED:0));
   auto *selected=catalog.choice(settings.family);auto label=selected?selected->label:catalog.view().automatic;
   add(fonts_recommended,Choose,locked|LYRA_SETTINGS_FORWARD,label);
   add(fonts_install,Import,locked|LYRA_SETTINGS_FORWARD);
   if(std::any_of(catalog.view().choices.begin(),catalog.view().choices.end(),[](const auto&c){return c.user;}))add(fonts_installed_title,Installed,locked|LYRA_SETTINGS_FORWARD);
   rows.back().flags|=LYRA_SETTINGS_GROUP_END;
   add(fonts_follow_parameters,Parameters,LYRA_SETTINGS_TOGGLE|LYRA_SETTINGS_GROUP_END|(settings.follow_parameters?LYRA_SETTINGS_CHECKED:0));
   const char *labels[]={font_styles_row_title_sp,font_styles_title_sp,font_styles_artist_sp,font_styles_duration_sp,font_styles_lyric_main_mini_sp,font_styles_lyric_main_expanded_sp};
   size_t label_index=0;for(auto role:lyra::custom_fonts::controls){auto i=size_t(role);auto label=labels[label_index++];
    auto name=std::string(label)+fonts_size;auto &s=add(name.c_str(),Size,controls|LYRA_SETTINGS_SLIDER);s.key=lyra::custom_fonts::size_keys[i];s.value=settings.sizes[i];s.minimum=settings.minimum[i];s.maximum=settings.maximum[i];s.step=1;
    name=std::string(label)+fonts_bold;auto &b=add(name.c_str(),Bold,controls|LYRA_SETTINGS_TOGGLE|(settings.bold[i]?LYRA_SETTINGS_CHECKED:0));b.key=lyra::custom_fonts::bold_keys[i];
    if(role==lyra::custom_fonts::FRowTitleSp || role==lyra::custom_fonts::FDurationSp || role==lyra::custom_fonts::FLyricMainMiniSp || role==lyra::custom_fonts::FLyricMainExpandedSp)b.flags|=LYRA_SETTINGS_GROUP_END;
   }
   add(fonts_reset,Reset,LYRA_SETTINGS_GROUP_END|(settings.modified?0:LYRA_SETTINGS_DISABLED));
  }else{
   title=page==1?fonts_recommended:fonts_installed_title;
   if(page==1){auto &a=add(fonts_automatic,Select,0,settings.family=="auto"?fonts_selected:"");a.id="auto";}
   for(const auto &c:catalog.view().choices)if(c.user==(page==2)){auto &a=add(c.label.c_str(),Select,locked,c.id==settings.family?fonts_selected:"");a.id=c.id;if(c.user)a.button=fonts_remove;}
   if(!rows.empty())rows.back().flags|=LYRA_SETTINGS_GROUP_END;
  }
  count=int64_t(rows.size());
 }
 void frame(){
  if(catalog.picking){int fd=font_pick_result(host);if(fd!=-1){catalog.picking=false;if(fd!=-2 && (fd<0 || !catalog.import_fd(fd,settings,const_cast<LyraHostV3*>(host),[](void*u){auto*h=static_cast<LyraHostV3*>(u);if(h->request_frame)h->request_frame(h->user);})))notice=strings::fonts_invalid;++revision;}}
  int imported=catalog.finish_import();if(imported){notice=imported>0?strings::fonts_installed:imported==lyra::ManagedFontCatalog::ImportLimit?strings::fonts_limit:strings::fonts_invalid;stamp.clear();}
  auto now=host && host->now_ns?host->now_ns(host->user):0;
  if(stamp.empty() || !polled_ns || now-polled_ns>=200000000){polled_ns=now;refresh();}
 }
 bool activate(int64_t index,bool secondary,const double *value){
  if(index<0 || size_t(index)>=rows.size() || !prompt.empty())return false;auto row=rows[size_t(index)];if(row.flags&LYRA_SETTINGS_DISABLED)return false;
  notice.clear();
  switch(row.action){
  case Follow:toggle("typography.follow_system");break;
  case Choose:page=1;++revision;build();break;
  case Installed:page=2;++revision;build();break;
  case Import:if(!catalog.picking){catalog.picking=font_pick(host,strings::fonts_install,settings.import_mime_types.c_str())==0;if(!catalog.picking){notice=strings::fonts_invalid;++revision;}}break;
  case Parameters:toggle("typography.follow_parameters");break;
  case Size:{if(!value || !std::isfinite(*value) || *value<row.minimum || *value>row.maximum)return false;char s[64];std::snprintf(s,sizeof(s),"%.6g",std::round(*value));write(row.key,s);break;}
  case Bold:write(row.key,(row.flags&LYRA_SETTINGS_CHECKED)?"false":"true");break;
  case Reset:reset=true;delete_id.clear();prompt=strings::fonts_reset_prompt;++revision;break;
  case Select:if(secondary && !row.button.empty()){reset=false;delete_id=row.id;prompt=strings::fonts_remove_prompt;++revision;}else if(write("typography.family",quote(row.id))){page=0;build();}break;
  default:return false;
  }
  if(host && host->request_frame)host->request_frame(host->user);return true;
 }
 bool confirm(bool yes){
  if(prompt.empty())return false;prompt.clear();++revision;
  if(yes){if(reset){for(int i=0;i<config->count(config->user);++i){LyraConfigDescriptionV3 d{};d.size=sizeof(d);if(config->describe(config->user,i,&d)==0 && mutable_key(d.key) && std::strcmp(d.value,d.default_value))write(d.key,d.default_value);}}
   else{bool selected=catalog.choice(settings.family)==catalog.choice(delete_id);if(catalog.remove(delete_id)){if(selected)write("typography.family","\"auto\"");stamp.clear();if(page==2 && std::none_of(catalog.view().choices.begin(),catalog.view().choices.end(),[](const auto&c){return c.user;}))page=0;}else notice="删除字体失败";}}
  delete_id.clear();return true;
 }
 static int request(void *user,const LyraMessageV3 *m,LyraReplyV3 *reply){
  if(!reply || reply->size!=sizeof(*reply))return LYRA_MESSAGE_INVALID;reply->field_count=0;reply->status=LYRA_MESSAGE_INVALID;
  const LyraFieldRuleV3 rules[]={{LYRA_SETTINGS_INDEX,LYRA_FIELD_I64},{LYRA_SETTINGS_SECONDARY,LYRA_FIELD_I64},{LYRA_SETTINGS_VALUE,LYRA_FIELD_F64},{LYRA_SETTINGS_ACCEPT,LYRA_FIELD_I64}};
  int status=lyra_message_fields(m,rules,std::size(rules));if(status)return reply->status=status;
  int64_t index=-1,secondary=0,accept=-1;double value=0;bool hasvalue=false;uint64_t seen=0;
  for(uint32_t i=0;i<m->field_count;++i){auto&f=m->fields[i];bool known=false;for(auto&r:rules)known|=r.tag==f.tag;if(!known)continue;if(!f.data || f.bytes!=sizeof(int64_t) || (seen&(1ull<<f.tag)))return reply->status;seen|=1ull<<f.tag;
   if(f.tag==LYRA_SETTINGS_INDEX)std::memcpy(&index,f.data,sizeof(index));if(f.tag==LYRA_SETTINGS_SECONDARY)std::memcpy(&secondary,f.data,sizeof(secondary));if(f.tag==LYRA_SETTINGS_ACCEPT)std::memcpy(&accept,f.data,sizeof(accept));if(f.tag==LYRA_SETTINGS_VALUE){std::memcpy(&value,f.data,sizeof(value));hasvalue=true;}}
  auto&s=*static_cast<Fonts*>(user);auto okay=[&]{return reply->status=LYRA_MESSAGE_OK;};
  switch(m->operation){
  case LYRA_SETTINGS_OPEN:s.page=0;s.prompt.clear();s.notice.clear();++s.revision;s.refresh();s.build();return okay();
  case LYRA_SETTINGS_BACK:if(!s.prompt.empty()){s.confirm(false);return okay();}if(!s.page)return reply->status=LYRA_MESSAGE_UNSUPPORTED;s.page=0;++s.revision;s.build();return okay();
  case LYRA_SETTINGS_ACTIVATE:if(secondary<0 || secondary>1 || !s.activate(index,secondary,hasvalue?&value:nullptr))return reply->status;return okay();
  case LYRA_SETTINGS_CONFIRM:if((accept!=0 && accept!=1) || !s.confirm(accept))return reply->status;return okay();
  case LYRA_SETTINGS_INFO:case LYRA_SETTINGS_ROW:break;
  default:return reply->status=LYRA_MESSAGE_UNSUPPORTED;
  }
  LyraFieldV3 fields[16];uint32_t n=0;
  auto text=[&](uint32_t tag,const std::string&v){fields[n++]={tag,LYRA_FIELD_UTF8,0,uint32_t(v.size()+1),v.c_str()};};
  auto integer=[&](uint32_t tag,const int64_t&v){fields[n++]={tag,LYRA_FIELD_I64,0,sizeof(v),&v};};
  auto real=[&](uint32_t tag,const double&v){fields[n++]={tag,LYRA_FIELD_F64,0,sizeof(v),&v};};
  if(m->operation==LYRA_SETTINGS_INFO){static const std::string entry=strings::fonts_title;text(LYRA_SETTINGS_ENTRY_TITLE,entry);text(LYRA_SETTINGS_TITLE,s.title);integer(LYRA_SETTINGS_COUNT,s.count);integer(LYRA_SETTINGS_PAGE,s.page);integer(LYRA_SETTINGS_REVISION,s.revision);text(LYRA_SETTINGS_PROMPT,s.prompt);text(LYRA_SETTINGS_NOTICE,s.notice);}
  else{if(index<0 || size_t(index)>=s.rows.size())return reply->status;auto&r=s.rows[size_t(index)];text(LYRA_SETTINGS_TITLE,r.title);text(LYRA_SETTINGS_DETAIL,r.detail);text(LYRA_SETTINGS_BUTTON,r.button);integer(LYRA_SETTINGS_FLAGS,r.flags);real(LYRA_SETTINGS_VALUE,r.value);real(LYRA_SETTINGS_MINIMUM,r.minimum);real(LYRA_SETTINGS_MAXIMUM,r.maximum);real(LYRA_SETTINGS_STEP,r.step);}
  if(reply->capacity<n || !reply->fields)return reply->status=LYRA_MESSAGE_CAPACITY;std::copy(fields,fields+n,reply->fields);reply->field_count=n;return okay();
 }
};
std::unique_ptr<Fonts> instance;
int init(const LyraHostV3 *h){
 if(!h || h->size!=sizeof(*h) || h->abi!=LYRA_PLUGIN_ABI || !h->query_interface || !font_platform_ready(h))return -1;
 auto *ui=static_cast<const LyraSettingsPageV3*>(h->query_interface(h->user,LYRA_INTERFACE_SETTINGS_PAGE,LYRA_INTERFACE_VERSION,sizeof(LyraSettingsPageV3)));
 if(!ui || ui->size!=sizeof(*ui) || ui->version!=LYRA_INTERFACE_VERSION || !ui->request)return -1;
 LyraMessageV3 message{sizeof(message),0,LYRA_SETTINGS_HOST_CAPABILITIES,nullptr};LyraFieldV3 field{};LyraReplyV3 reply{sizeof(reply),1,0,0,&field};
 if(ui->request(ui->user,&message,&reply)!=0 || reply.status || reply.field_count!=1 || field.tag!=LYRA_SETTINGS_FLAGS || field.type!=LYRA_FIELD_I64 || field.bytes!=sizeof(int64_t) || !field.data)return -1;
 int64_t flags=0;std::memcpy(&flags,field.data,sizeof(flags));if(!(flags&LYRA_SETTINGS_SLIDER) || !(flags&LYRA_SETTINGS_TOGGLE))return -1;
 auto s=std::make_unique<Fonts>();s->host=h;
 s->config=static_cast<const LyraConfigInterfaceV3*>(h->query_interface(h->user,LYRA_INTERFACE_CONFIG,LYRA_INTERFACE_VERSION,sizeof(LyraConfigInterfaceV3)));
 s->schema=static_cast<const LyraConfigSchemaV3*>(h->query_interface(h->user,LYRA_INTERFACE_CONFIG_SCHEMA,LYRA_INTERFACE_VERSION,sizeof(LyraConfigSchemaV3)));
 if(!s->config || !s->config->write || !s->config->count || !s->config->describe || s->config->size!=sizeof(*s->config) || s->config->version!=LYRA_INTERFACE_VERSION || !s->schema || !s->schema->text || s->schema->size!=sizeof(*s->schema) || s->schema->version!=LYRA_INTERFACE_VERSION)return -1;
 s->root=font_storage(h);if(s->root.empty())return -1;s->catalog.user_root(s->root);s->refresh();
 s->api={sizeof(s->api),LYRA_INTERFACE_VERSION,s.get(),Fonts::request};
 s->manager={sizeof(s->manager),LYRA_INTERFACE_VERSION,s.get(),[](void *u,const LyraMessageV3*m,LyraReplyV3*r){
  if(!r || r->size!=sizeof(*r))return LYRA_MESSAGE_INVALID;r->field_count=0;r->status=LYRA_MESSAGE_INVALID;
  const LyraFieldRuleV3 rule{LYRA_FONT_CHOICE,LYRA_FIELD_UTF8};int status=lyra_message_fields(m,&rule,1);if(status)return r->status=status;
  if(m->operation!=LYRA_FONT_REMOVE)return r->status=LYRA_MESSAGE_UNSUPPORTED;
  const LyraFieldV3 *choice=nullptr;for(uint32_t i=0;i<m->field_count;++i)if(m->fields[i].tag==LYRA_FONT_CHOICE){if(choice)return r->status;choice=&m->fields[i];}
  auto &s=*static_cast<Fonts*>(u);if(!choice || !choice->data || !choice->bytes || choice->bytes>4096 || s.settings.follow_system || std::memchr(choice->data,0,choice->bytes)!=static_cast<const char*>(choice->data)+choice->bytes-1)return r->status;
  auto id=std::string(static_cast<const char*>(choice->data),choice->bytes-1);const auto *target=s.catalog.choice(id);if(!target || !target->user)return r->status;
  if(!r->capacity || !r->fields)return r->status=LYRA_MESSAGE_CAPACITY;
  s.reply_integer=s.catalog.choice(s.settings.family)==target;
  if(!s.catalog.remove(id))return r->status;
  // Legacy manager clients retain ownership of persisting automatic selection.
  s.stamp.clear();++s.revision;r->fields[0]={LYRA_FONT_SELECTED,LYRA_FIELD_I64,0,sizeof(s.reply_integer),&s.reply_integer};r->field_count=1;
  if(s.host && s.host->request_frame)s.host->request_frame(s.host->user);return r->status=LYRA_MESSAGE_OK;
 }};
 instance=std::move(s);return 0;
}
}
extern "C" __attribute__((visibility("default"))) int lyra_plugin_init(const LyraHostV3*h,LyraPluginV3*out){
 if(!out || !h || h->size!=sizeof(*h) || h->abi!=LYRA_PLUGIN_ABI)return -1;
 *out={};out->size=sizeof(*out);out->abi=LYRA_PLUGIN_ABI;out->id="official.fonts";out->kind="font";out->version="1.0.0";out->config_tables="typography";out->init=init;
 out->shutdown=[](const LyraHostV3*){instance.reset();};out->on_frame=[](const LyraHostV3*,int64_t){if(instance)instance->frame();return 0;};
 out->supply=[](const LyraHostV3*,int64_t id,char *out,int cap){if(!instance)return 0;if(instance->settings.follow_system){const char *s="sans-serif";if(!out || cap<=int(std::strlen(s)))return 0;std::strcpy(out,s);return int(std::strlen(s));}return instance->catalog.supply(id,out,cap);};
 out->query_interface=[](const LyraHostV3*,uint32_t id,uint32_t version,uint32_t size)->const void*{if(!instance || version!=LYRA_INTERFACE_VERSION)return nullptr;if(id==LYRA_INTERFACE_SETTINGS_PAGE && size==sizeof(LyraSettingsPageV3))return &instance->api;if(id==LYRA_INTERFACE_FONT_MANAGER && size==sizeof(LyraFontManagerV3))return &instance->manager;return nullptr;};return 0;
}
