#include "engine.h"
#include "platform.h"
#include "json.hpp"
#include "plugin/collection_directory_contracts.h"
#include "plugin/media_contracts.h"
#include "plugin/settings_contracts.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {
using Json=nlohmann::json;
using namespace sources;
constexpr char id[]="official.sources";
constexpr char kind[]="media.sources";
constexpr size_t source_limit=12,item_limit=100,script_limit=1024*1024;
const LyraHostV3 *host=nullptr;
struct Source { std::string id,script,url;Json metadata;bool enabled=true; };
struct Job { uint64_t request=0;std::function<void()> run; };
std::vector<Source> sources_list;
std::deque<Job> jobs;
std::vector<std::thread> workers;
std::mutex lock;
std::condition_variable ready;
std::atomic<bool> stopping{false};
std::string path,notice,detail,target;
enum class Prompt { closed,import,remove };
Prompt prompt=Prompt::closed;
bool choosing=false;
int64_t revision=1;
uint64_t serial=0;
uint64_t directory=0;
LyraMediaSinkV3 directory_sink{};
std::string str(const Json &object,const char *key){
    if(!object.is_object())return {};
    auto found=object.find(key);
    if(found==object.end())return {};
    if(found->is_string())return found->get<std::string>();
    return found->is_number()?found->dump():std::string{};
}
Json parse(std::string_view raw){return Json::parse(raw.begin(),raw.end(),nullptr,false);}
std::string display(const Source &source){auto name=str(source.metadata,"name");return name.empty()?source.id:name;}
std::string meta_header(std::string_view script,std::string_view key){
    auto header=script.substr(0,script.find("*/"));
    std::string field="@"+std::string(key)+" ";
    auto at=header.find(field);
    if(at==header.npos)return {};
    auto value=header.substr(at+field.size());value=value.substr(0,value.find_first_of("\r\n"));
    return std::string(value.substr(0,256));
}
void wake(){if(host && host->request_frame)host->request_frame(host->user);}
bool save(){
    Json data=Json::array();
    for(const auto &s:sources_list)data.push_back({{"id",s.id},{"url",s.url},{"script",s.script},{"enabled",s.enabled},{"metadata",s.metadata}});
    auto temp=path+".tmp";
    auto file=std::fopen(temp.c_str(),"wb");if(!file)return false;
    auto text=data.dump(-1,' ',false,Json::error_handler_t::replace);
    bool ok=std::fwrite(text.data(),1,text.size(),file)==text.size() && std::fflush(file)==0 && fsync(fileno(file))==0;
    if(std::fclose(file)!=0)ok=false;
    if(ok && std::rename(temp.c_str(),path.c_str())==0)return true;
    unlink(temp.c_str());return false;
}
void restore(){
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file || file.tellg()>std::streamoff(source_limit*script_limit*2))return;
    std::string data(size_t(file.tellg()),'\0');file.seekg(0);if(!file.read(data.data(),std::streamsize(data.size())))return;
    auto value=parse(data);if(!value.is_array())return;
    for(auto &item:value){
        if(sources_list.size()>=source_limit || !item.is_object())break;
        auto key=str(item,"id"),script=str(item,"script");
        if(key.empty() || script.empty() || script.size()>script_limit || !item.contains("metadata") || !item["metadata"].is_object())continue;
        auto number=std::strtoull(key.c_str(),nullptr,10);if(number>=UINT64_MAX-1)continue;
        serial=std::max(serial,uint64_t(number));
        sources_list.push_back({std::move(key),std::move(script),str(item,"url"),item["metadata"],
            !item.contains("enabled") || item["enabled"]!=false});
    }
}
bool enqueue(Job job){
    if(stopping || jobs.size()>=24)return false;
    jobs.push_back(std::move(job));ready.notify_one();return true;
}
void worker(){
    for(;;){
        Job job;
        {std::unique_lock guard(lock);ready.wait(guard,[]{return stopping || !jobs.empty();});
            if(stopping)return;
            job=std::move(jobs.front());jobs.pop_front();}
        try { job.run(); }
        catch (...) { }
    }
}
Json invoke(const Source &source,const char *operation,const Json &args){
    return parse(evaluate(source.script,operation,args.dump(),network));
}
struct Row {std::string uid,title,artist,album,cover,lyric;int64_t duration=0;};
void publish(uint64_t request,LyraMediaSinkV3 sink,const std::vector<Row> &rows,int error=0){
    if(stopping)return;
    std::vector<LyraMediaDescriptorV3> items;
    for(auto &row:rows){
        LyraMediaDescriptorV3 item{};item.size=sizeof(item);item.uid=row.uid.c_str();item.title=row.title.c_str();
        item.artist=row.artist.c_str();item.album=row.album.c_str();item.cover_uri=row.cover.c_str();
        item.lyrics_text=row.lyric.c_str();item.duration_us=row.duration;items.push_back(item);
    }
    LyraMediaBatchV3 batch{sizeof(batch),LYRA_MEDIA_COMPLETE,request,items.data(),uint32_t(items.size()),nullptr,error};
    sink.publish(sink.user,&batch);
}
std::string encode_id(const Json &value){return value.dump(-1,' ',false,Json::error_handler_t::replace);}
void add_track(std::vector<Row> &out,const Source &source,const Json &song){
    if(out.size()>=item_limit || !song.is_object())return;
    auto title=str(song,"title"),artist=str(song,"artist");
    auto identity=str(song,"id");
    if(title.empty() || identity.empty())return;
    // Save the provider's opaque IDs and primitive metadata so favorites can resolve after restart.
    Json input=Json::object();
    for(auto &[key,value]:song.items()) {
        if(key.size()<=64 && (value.is_number() || value.is_boolean() ||
            (value.is_string() && value.get_ref<const std::string &>().size()<=256)))input[key]=value;
    }
    auto uid=encode_id(Json::array({source.id,input}));
    if(uid.size()>1800 || std::any_of(out.begin(),out.end(),[&](const Row &row){return row.uid==uid;}))return;
    int64_t duration=0;
    auto seconds=song.find("duration");
    if(seconds!=song.end() && seconds->is_number()){
        double n=seconds->get<double>();if(n>0 && n<86400)duration=int64_t(n*1000000);
    }
    out.push_back({std::move(uid),std::move(title),std::move(artist),str(song,"album"),str(song,"artwork"),{},duration});
}
void add_tracks(std::vector<Row> &out,const Source &source,const Json &result){
    if(result.is_array()){for(auto &item:result)add_track(out,source,item);return;}
    if(!result.is_object())return;
    for(const char *key:{"data","musicList","list"})
        if(result.contains(key) && result[key].is_array()){add_tracks(out,source,result[key]);return;}
}
std::vector<Source> enabled_sources(){
    std::lock_guard guard(lock);std::vector<Source> active;
    for(const auto &source:sources_list)if(source.enabled)active.push_back(source);return active;
}
std::vector<Row> collection_rows(const std::vector<Source> &active){
    std::vector<Row> out;
    for(const auto &source:active){
        auto result=invoke(source,"collections",Json::array());
        if(!result.is_array())continue;
        for(const auto &category:result){
            const auto &lists=category.is_object() && category.contains("data")?category["data"]:Json::array({category});
            if(!lists.is_array())continue;
            for(const auto &entry:lists){
                auto name=str(entry,"title");if(name.empty() || str(entry,"id").empty() || out.size()>=item_limit)continue;
                auto uid=encode_id(Json::array({source.id,entry}));
                if(uid.size()<=1800 && std::none_of(out.begin(),out.end(),[&](const Row &r){return r.uid==uid;}))
                    out.push_back({std::move(uid),std::move(name),{},{},{},{},0});
            }
        }
    }
    return out;
}
void refresh_directory(){
    uint64_t request;LyraMediaSinkV3 sink;
    {std::lock_guard guard(lock);request=directory;sink=directory_sink;}
    if(request)publish(request,sink,collection_rows(enabled_sources()));
}
void import_script(std::string data,std::string url,std::string update){
    if(data.empty() && !url.empty()){
        auto result=parse(network(url,"GET","{}",""));data=str(result,"body");
    }
    auto metadata=parse(evaluate(data,"metadata","[]",network));
    if(metadata.is_object() && str(metadata,"format")=="lx")
        for(const char *key:{"name","version","description"})metadata[key]=meta_header(data,key);
    if(metadata.is_object()){
        auto icon=str(metadata,"icon");
        if(!icon.empty())metadata["tile"]=icon_tile(icon);
        metadata.erase("icon");
    }
    bool changed=false;
    {
        std::lock_guard guard(lock);
        if(!metadata.is_object())notice="无法识别音源或脚本缺少依赖";
        else if(update.empty() && sources_list.size()>=source_limit)notice="音源数量已达上限";
        else {
            auto old=std::find_if(sources_list.begin(),sources_list.end(),[&](const Source &s){
                return !update.empty()?s.id==update:(!url.empty() && s.url==url) || s.script==data;
            });
            if(!update.empty() && old==sources_list.end())return;
            if(old==sources_list.end()){
                sources_list.push_back({std::to_string(++serial),std::move(data),std::move(url),std::move(metadata)});
                changed=save();
                if(!changed)sources_list.pop_back();
            }else{
                Source backup=*old;old->script=std::move(data);old->metadata=std::move(metadata);
                changed=save();if(!changed)*old=std::move(backup);
            }
            notice=changed?"音源已保存":"无法保存音源";
        }
        ++revision;
    }
    wake();
    if(changed)refresh_directory();
}
std::string lx_platform(const Json &song){
    auto value=str(song,"source");if(value.empty())value=str(song,"platform");
    for(const auto *name:{"kw","kg","tx","wy","mg","local"})if(value==name)return value;
    if(value=="网易云" || value=="网易云音乐")return "wy";
    if(value=="QQ音乐")return "tx";
    if(value=="酷狗音乐")return "kg";
    if(value=="酷我音乐")return "kw";
    if(value=="咪咕音乐")return "mg";
    return {};
}
void resolve(uint64_t request,LyraMediaSinkV3 sink,uint32_t operation,const std::vector<Source> &active,const Json &uid){
    if(!uid.is_array() || uid.size()!=2 || !uid[0].is_string() || !uid[1].is_object()){
        LyraMediaResourceV3 error{sizeof(error),request,-1,-1,nullptr,nullptr,nullptr};sink.resolved(sink.user,&error);return;
    }
    auto platform=lx_platform(uid[1]);
    struct Race {std::mutex mutex;std::condition_variable done;size_t pending=0;std::string uri,headers;};
    Race race;
    std::vector<std::future<void>> attempts;
    for(const auto &source:active){
        const bool primary=source.id==uid[0];
        const bool lx=str(source.metadata,"format")=="lx";
        if(!primary && operation!=LYRA_MEDIA_AUDIO)continue;
        if(lx && platform.empty())continue;
        {std::lock_guard guard(race.mutex);++race.pending;}
        attempts.push_back(std::async(std::launch::async,[&,source,primary,lx]{
            std::string url,headers;
            try {
                auto song=uid[1];
                if(!primary && !lx){
                    auto found=invoke(source,"search",Json::array({str(song,"title")}));
                    if(found.is_object() && found.contains("data"))found=Json(found["data"]);
                    bool matched=false;
                    if(found.is_array())for(const auto &item:found){
                        auto name=str(song,"title"),artist=str(song,"artist"),length=str(song,"duration");
                        if(!name.empty() && !artist.empty() && !length.empty() && str(item,"title")==name &&
                           str(item,"artist")==artist && str(item,"duration")==length &&
                           str(item,"album")==str(song,"album")){song=item;matched=true;break;}
                    }
                    if(!matched)song=nullptr;
                }
                auto result=song.is_null()?Json{}:invoke(source,"resolve",Json::array({platform,song}));
                url=str(result,"url");
                if(url.empty() && result.is_string())url=result.get<std::string>();
                if(result.is_object() && result.contains("headers") && result["headers"].is_object())headers=result["headers"].dump();
                if(url.empty() && primary && operation==LYRA_MEDIA_AUDIO)url=str(uid[1],"url");
            }catch(...){}
            std::lock_guard guard(race.mutex);
            if(race.uri.empty() && url.substr(0,8)=="https://"){race.uri=std::move(url);race.headers=std::move(headers);}
            --race.pending;race.done.notify_one();
        }));
    }
    std::unique_lock guard(race.mutex);
    race.done.wait(guard,[&]{return !race.uri.empty() || race.pending==0;});
    LyraMediaResourceV3 result{sizeof(result),request,-1,race.uri.empty()?-1:0,race.uri.c_str(),race.headers.c_str(),nullptr};
    if(!stopping)sink.resolved(sink.user,&result);
    guard.unlock();
}
int media_request(void *,const LyraMediaRequestV3 *message,const LyraMediaSinkV3 *sink){
    if(!message || message->size!=sizeof(*message) || !sink || sink->size!=sizeof(*sink))return -1;
    auto request=message->request_id;auto output=*sink;auto operation=message->kind;
    const char *value=operation==LYRA_MEDIA_SEARCH?message->query:operation==LYRA_MEDIA_COLLECTION?message->collection:message->uid;
    std::string input=value?value:"";
    if(operation!=LYRA_MEDIA_SEARCH && operation!=LYRA_MEDIA_COLLECTION && operation!=LYRA_MEDIA_AUDIO && operation!=LYRA_MEDIA_LYRICS)return -1;
    std::lock_guard guard(lock);
    if(operation==LYRA_MEDIA_COLLECTION && input.empty()){directory=request;directory_sink=output;}
    return enqueue({request,[request,output,operation,input=std::move(input)]{
        auto active=enabled_sources();
        std::vector<Row> rows;
        if(operation==LYRA_MEDIA_AUDIO){resolve(request,output,operation,active,parse(input));return;}
        if(operation==LYRA_MEDIA_LYRICS){
            auto uid=parse(input);int fd=-1;
            if(uid.is_array() && uid.size()==2)for(const auto &source:active)if(source.id==uid[0]){
                auto result=invoke(source,"lyrics",Json::array({uid[1]}));
                auto lyrics=result.is_string()?result.get<std::string>():str(result,"rawLrc");
                if(!lyrics.empty())if(auto *file=std::tmpfile()){
                    if(std::fwrite(lyrics.data(),1,lyrics.size(),file)==lyrics.size() && std::fflush(file)==0){
                        std::rewind(file);fd=dup(fileno(file));
                    }
                    std::fclose(file);
                }
                break;
            }
            LyraMediaResourceV3 result{sizeof(result),request,fd,fd<0?-1:0,nullptr,nullptr,nullptr};
            if(!stopping)output.resolved(output.user,&result);else if(fd>=0)close(fd);
            return;
        }
        if(operation==LYRA_MEDIA_SEARCH){
            for(const auto &source:active)add_tracks(rows,source,invoke(source,"search",Json::array({input})));
        }else if(input.empty())rows=collection_rows(active);
        else {
            auto key=parse(input);
            if(key.is_array() && key.size()==2)
                for(const auto &source:active)if(source.id==key[0])add_tracks(rows,source,invoke(source,"collection",Json::array({key[1]})));
        }
        publish(request,output,rows);
    }})?0:-1;
}
void media_cancel(void *,uint64_t request){
    std::lock_guard guard(lock);
    if(directory==request)directory=0;
    jobs.erase(std::remove_if(jobs.begin(),jobs.end(),[&](const Job &job){return job.request==request;}),jobs.end());
}
const LyraMediaProviderV3 provider{sizeof(provider),LYRA_INTERFACE_VERSION,nullptr,id,media_request,media_cancel};
int directory_info(void *,const LyraMessageV3 *message,LyraReplyV3 *reply){
    if(!message || message->size!=sizeof(*message) || !reply || reply->size!=sizeof(*reply))return LYRA_MESSAGE_INVALID;
    reply->field_count=0;
    if(message->operation!=LYRA_COLLECTION_DIRECTORY_INFO)return reply->status=LYRA_MESSAGE_UNSUPPORTED;
    if(!reply->capacity || !reply->fields)return reply->status=LYRA_MESSAGE_CAPACITY;
    reply->fields[0]={LYRA_COLLECTION_DIRECTORY_PROVIDER,LYRA_FIELD_UTF8,0,sizeof(id),id};
    reply->field_count=1;return reply->status=LYRA_MESSAGE_OK;
}
const LyraCollectionDirectoryV3 collections{sizeof(collections),LYRA_INTERFACE_VERSION,nullptr,directory_info};
struct SettingsReply {
    LyraReplyV3 *reply;
    std::array<std::string,16> words;
    std::array<int64_t,16> numbers;
    void string(uint32_t key,std::string value){
        auto i=reply->field_count;if(i>=reply->capacity || i>=words.size())return;
        words[i]=std::move(value);reply->fields[i]={key,LYRA_FIELD_UTF8,0,uint32_t(words[i].size()+1),words[i].c_str()};++reply->field_count;
    }
    void image(std::string_view encoded){
        auto i=reply->field_count;
        if(i>=reply->capacity || i>=words.size() || encoded.size()!=LYRA_SETTINGS_TILE_SIDE*LYRA_SETTINGS_TILE_SIDE*8)return;
        auto digit=[](char c)->unsigned{return c>='a'?unsigned(c-'a'+10):unsigned(c-'0');};
        words[i].resize(encoded.size()/2);
        for(size_t p=0;p<words[i].size();++p)words[i][p]=char((digit(encoded[p*2])<<4)|digit(encoded[p*2+1]));
        reply->fields[i]={LYRA_SETTINGS_LEADING_RGBA,LYRA_FIELD_BYTES,0,uint32_t(words[i].size()),words[i].data()};
        ++reply->field_count;
    }
    void number(uint32_t key,int64_t value){
        auto i=reply->field_count;if(i>=reply->capacity || i>=numbers.size())return;
        numbers[i]=value;reply->fields[i]={key,LYRA_FIELD_I64,0,sizeof(value),&numbers[i]};++reply->field_count;
    }
};
int64_t number(const LyraMessageV3 *msg,uint32_t key){
    auto *f=lyra_message_field(msg,key);int64_t n=0;
    if(f && f->type==LYRA_FIELD_I64 && f->bytes==sizeof(n) && f->data)std::memcpy(&n,f->data,sizeof(n));return n;
}
std::string text(const LyraMessageV3 *msg,uint32_t key){
    auto *f=lyra_message_field(msg,key);
    if(!f || f->type!=LYRA_FIELD_UTF8 || !f->data || !f->bytes || f->bytes>8192)return {};
    auto *value=static_cast<const char *>(f->data);
    return std::memchr(value,0,f->bytes)==value+f->bytes-1?value:std::string{};
}
std::string initial(std::string name){
    if(name.empty())return "?";
    size_t n=1;while(n<name.size() && (static_cast<unsigned char>(name[n])&0xc0)==0x80)++n;
    return name.substr(0,n);
}
int settings(void *,const LyraMessageV3 *message,LyraReplyV3 *reply){
    if(!message || message->size!=sizeof(*message) || !reply || reply->size!=sizeof(*reply))return LYRA_MESSAGE_INVALID;
    reply->field_count=0;
    static thread_local SettingsReply out{};
    out.reply=reply;
    std::lock_guard guard(lock);
    auto active=std::find_if(sources_list.begin(),sources_list.end(),[](const Source &s){return s.id==detail;});
    const bool details=active!=sources_list.end();
    auto row=number(message,LYRA_SETTINGS_INDEX);
    switch(message->operation){
    case LYRA_SETTINGS_INFO:
        if(choosing){
            int fd=take_document();
            if(fd!=-1){
                choosing=false;prompt=Prompt::closed;++revision;
                if(fd>=0 && !enqueue({0,[fd]{import_script(read_file(fd,script_limit),{},{});}}))close(fd);
            }
        }
        out.string(LYRA_SETTINGS_TITLE,details?display(*active):"导入音源");
        out.string(LYRA_SETTINGS_ENTRY_TITLE,"导入音源");
        out.string(LYRA_SETTINGS_ENTRY_PARENT,"settings.scan");
        out.number(LYRA_SETTINGS_INLINE,1);
        out.number(LYRA_SETTINGS_COUNT,details?5:int64_t(sources_list.size()+1));
        out.number(LYRA_SETTINGS_PAGE,details?1:0);
        out.number(LYRA_SETTINGS_REVISION,revision);
        if(!notice.empty())out.string(LYRA_SETTINGS_NOTICE,notice);
        if(prompt!=Prompt::closed){
            out.string(LYRA_SETTINGS_PROMPT,prompt==Prompt::import?"导入音源":"删除此音源？");
            if(prompt==Prompt::import){out.string(LYRA_SETTINGS_INPUT,"");out.string(LYRA_SETTINGS_PROMPT_ACTION,"选择文件");}
        }
        break;
    case LYRA_SETTINGS_ROW:
        if(row<0 || row>=(details?5:int64_t(sources_list.size()+1)))return reply->status=LYRA_MESSAGE_INVALID;
        if(details){
            const char *titles[]={"启用","版本","说明","能力","更新音源"};
            out.string(LYRA_SETTINGS_TITLE,titles[row]);
            if(row==0)out.number(LYRA_SETTINGS_FLAGS,LYRA_SETTINGS_TOGGLE|(active->enabled?LYRA_SETTINGS_CHECKED:0));
            if(row==1)out.string(LYRA_SETTINGS_DETAIL,str(active->metadata,"version"));
            if(row==2)out.string(LYRA_SETTINGS_DETAIL,str(active->metadata,"description"));
            if(row==3 && active->metadata.contains("capabilities")){
                std::string label;
                for(const auto &capability:active->metadata["capabilities"]){
                    if(!capability.is_string())continue;
                    if(!label.empty())label+=" · ";
                    label+=capability.get<std::string>();
                }
                out.string(LYRA_SETTINGS_DETAIL,std::move(label));
            }
        }else if(!row)out.string(LYRA_SETTINGS_TITLE,"导入音源");
        else{
            auto &s=sources_list[size_t(row-1)];
            out.string(LYRA_SETTINGS_TITLE,display(s));
            out.string(LYRA_SETTINGS_DETAIL,s.enabled?"已启用":"已停用");
            out.string(LYRA_SETTINGS_LEADING_TEXT,initial(display(s)));
            out.image(str(s.metadata,"tile"));
            out.string(LYRA_SETTINGS_BUTTON,"删除");
        }
        break;
    case LYRA_SETTINGS_OPEN: detail.clear();notice.clear();++revision;break;
    case LYRA_SETTINGS_BACK:
        if(!details)return reply->status=LYRA_MESSAGE_UNSUPPORTED;
        detail.clear();++revision;break;
    case LYRA_SETTINGS_ACTIVATE:
        notice.clear();
        if(details){
            if(row==0){active->enabled=!active->enabled;if(!save())active->enabled=!active->enabled;else enqueue({0,refresh_directory});}
            if(row==4){
                auto url=str(active->metadata,"update");if(url.empty())url=active->url;
                if(url.empty())notice="此音源没有更新链接";
                else enqueue({0,[url,key=active->id]{import_script({},url,key);}});
            }
        }else if(!row)prompt=Prompt::import;
        else if(row>0 && size_t(row)<=sources_list.size()){
            auto &s=sources_list[size_t(row-1)];
            if(number(message,LYRA_SETTINGS_SECONDARY)){target=s.id;prompt=Prompt::remove;}else detail=s.id;
        }
        ++revision;break;
    case LYRA_SETTINGS_ACTIVATE_PROMPT:
        if(prompt!=Prompt::import || choosing)return reply->status=LYRA_MESSAGE_INVALID;
        choosing=choose_document();
        if(!choosing)notice="无法打开文件选择器";
        ++revision;break;
    case LYRA_SETTINGS_CONFIRM:
        if(number(message,LYRA_SETTINGS_ACCEPT)){
            if(prompt==Prompt::import){
                auto url=text(message,LYRA_SETTINGS_INPUT);
                if(url.substr(0,8)!="https://")notice="请输入 HTTPS 音源链接";
                else enqueue({0,[url]{import_script({},url,{});}});
            }else if(prompt==Prompt::remove){
                auto found=std::find_if(sources_list.begin(),sources_list.end(),[](const Source &s){return s.id==target;});
                if(found!=sources_list.end()){
                    auto removed=std::move(*found);auto position=found-sources_list.begin();sources_list.erase(found);
                    if(!save())sources_list.insert(sources_list.begin()+position,std::move(removed));
                    else enqueue({0,refresh_directory});
                }
            }
        }
        prompt=Prompt::closed;++revision;break;
    default:return reply->status=LYRA_MESSAGE_UNSUPPORTED;
    }
    return reply->status=LYRA_MESSAGE_OK;
}
const LyraSettingsPageV3 page{sizeof(page),LYRA_INTERFACE_VERSION,nullptr,settings};
const void *query(const LyraHostV3 *,uint32_t interface,uint32_t version,uint32_t size){
    if(version!=LYRA_INTERFACE_VERSION)return nullptr;
    if(interface==LYRA_INTERFACE_MEDIA_PROVIDER && size==sizeof(provider))return &provider;
    if(interface==LYRA_INTERFACE_SETTINGS_PAGE && size==sizeof(page))return &page;
    if(interface==LYRA_INTERFACE_COLLECTION_DIRECTORY && size==sizeof(collections))return &collections;
    return nullptr;
}
int start(const LyraHostV3 *value){
    if(!initialize(value))return -1;
    auto root=files_directory();if(root.empty()){disconnect();return -1;}
    host=value;path=root+"/official.sources.json";stopping=false;
    restore();
    for(int i=0;i<2;++i)workers.emplace_back(worker);
    return 0;
}
void stop(const LyraHostV3 *){
    {std::lock_guard guard(lock);stopping=true;jobs.clear();}
    ready.notify_all();
    for(auto &thread:workers)thread.join();
    workers.clear();sources_list.clear();directory=0;host=nullptr;disconnect();
}
}
extern "C" __attribute__((visibility("default"))) int lyra_plugin_init(const LyraHostV3 *host,LyraPluginV3 *out){
    if(!host || host->size!=sizeof(*host) || host->abi!=LYRA_PLUGIN_ABI || !out)return -1;
    *out={};out->size=sizeof(*out);out->abi=LYRA_PLUGIN_ABI;out->id=id;out->kind=kind;
    out->version="1.0.0";out->init=start;out->shutdown=stop;out->query_interface=query;
    return 0;
}
