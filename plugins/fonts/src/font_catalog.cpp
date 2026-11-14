#include "font_catalog.h"

#include "font_log.h"
#include "font_metadata.h"
#include <fcntl.h>
#include <unistd.h>
#include <map>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace lyra {
namespace {

const char *const kFontDirs[] = {
    "/system/fonts", "/product/fonts", "/system_ext/fonts",
    "/vendor/fonts", "/odm/fonts", "/data/fonts/files", "/data/fonts",
};

// Lower rank wins. The order is the selection priority.
enum class Rank {
    PingFangSC,
    PingFangDefault,
    PingFangVariant,
    MiSansVariable,
    MiSansL3,
    MiSansC,
    MiSansPlain,
    HarmonyOs,
    NotoCjk,
    NotoSc,
    Roboto,
    None,
};

struct Face {
    std::string path;
    int weight = 0;
    int order = 0;
};

std::string lower_ascii(const char *name) {
    std::string out(name);
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return out;
}

bool contains(const std::string &text, const char *needle) {
    return text.find(needle) != std::string::npos;
}

bool starts_with(const std::string &text, const char *prefix) {
    size_t n = std::strlen(prefix);
    return text.size() >= n && text.compare(0, n, prefix) == 0;
}

bool font_name(const std::string &lower) {
    size_t dot = lower.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = lower.substr(dot);
    return ext == ".ttf" || ext == ".otf" || ext == ".ttc";
}

bool usable_text_name(const std::string &lower) {
    return !contains(lower, "italic") && !contains(lower, "oblique") &&
           !contains(lower, "overlay") && !contains(lower, "emoji") &&
           !contains(lower, "symbol") && !contains(lower, "clock") &&
           !contains(lower, "misansrounded") && !contains(lower, "misanssquare") &&
           !contains(lower, "misanserif") && !contains(lower, "misanslatin") &&
           !contains(lower, "misansjapanese") && !contains(lower, "misanskorean") &&
           !contains(lower, "misansarabic") && !contains(lower, "misansdevanagari") &&
           !contains(lower, "misansthai") && !contains(lower, "misanstibetan") &&
           !contains(lower, "misanslao") && !contains(lower, "misansmyanmar");
}

bool pingfang(const std::string &lower, Rank *rank) {
    if (!contains(lower, "pingfang")) return false;
    if (contains(lower, "pingfangsc")) *rank = Rank::PingFangSC;
    else if (contains(lower, "pingfangtc") || contains(lower, "pingfanghk") ||
             contains(lower, "pingfangmo"))
        *rank = Rank::PingFangVariant;
    else
        *rank = Rank::PingFangDefault;
    return true;
}

bool classify(const std::string &n, Rank *rank) {
    if (pingfang(n, rank)) return true;
    if (starts_with(n, "misansvf")) *rank = Rank::MiSansVariable;
    else if (starts_with(n, "misansl3") || starts_with(n, "misans-l3")) *rank = Rank::MiSansL3;
    else if (starts_with(n, "misansc")) *rank = Rank::MiSansC;
    else if (starts_with(n, "misans-") || starts_with(n, "misans_")) *rank = Rank::MiSansPlain;
    else if (starts_with(n, "harmonyos_sans_sc")) *rank = Rank::HarmonyOs;
    else if (starts_with(n, "notosanscjk")) *rank = Rank::NotoCjk;
    else if (starts_with(n, "notosanssc")) *rank = Rank::NotoSc;
    else if (starts_with(n, "roboto") && !contains(n, "condensed") &&
             !contains(n, "mono") && !contains(n, "serif") && !contains(n, "flex"))
        *rank = Rank::Roboto;
    else
        return false;
    return true;
}

const char *family_label(Rank rank) {
    if (rank < Rank::MiSansVariable) return "PingFang";
    if (rank < Rank::HarmonyOs) return "MiSans";
    if (rank < Rank::NotoCjk) return "HarmonyOS Sans";
    if (rank < Rank::Roboto) return "Noto Sans";
    return "Roboto";
}

// Only the four weights this app renders are collected. Medium and semibold
// files are skipped instead of being mislabeled.
bool weight_for(const std::string &n, int regular, int light, int bold, int heavy, int *out) {
    if (contains(n, "ultralight") || contains(n, "extralight") || contains(n, "thin") ||
        contains(n, "semilight") || contains(n, "light")) {
        *out = light;
        return light > 0;
    }
    if (contains(n, "demibold") || contains(n, "semibold") || contains(n, "medium")) return false;
    if (contains(n, "extrabold") || contains(n, "ultrabold") || contains(n, "black") ||
        contains(n, "heavy")) {
        *out = heavy;
        return heavy > 0;
    }
    if (contains(n, "bold")) {
        *out = bold;
        return bold > 0;
    }
    *out = regular;
    return regular > 0;
}

void read_dir(const char *dir, std::vector<ManagedFontCatalog::InstalledFont> &out, int *seen) {
    DIR *handle = opendir(dir);
    if (handle == nullptr) return;
    while (dirent *entry = readdir(handle)) {
        if (entry->d_name[0] == '.') continue;
        std::string lower = lower_ascii(entry->d_name);
        if (!font_name(lower)) continue;
        std::string path = std::string(dir) + "/" + entry->d_name;
        if (path.find('|') != std::string::npos || path.find('#') != std::string::npos) continue;
        struct stat info{};
        if (lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0) continue;
        ++*seen;
        out.push_back({path, lower});
    }
    closedir(handle);
}

std::string build_spec(std::vector<Face> faces) {
    std::sort(faces.begin(), faces.end(), [](const Face &a, const Face &b) {
        if (a.weight != b.weight) return a.weight < b.weight;
        return a.order < b.order;
    });
    std::string spec = "lyra:";
    int last_weight = 0;
    bool first = true;
    for (const Face &face : faces) {
        if (face.weight == last_weight) continue;
        if (!first) spec.push_back('|');
        spec += std::to_string(face.weight);
        spec.push_back('=');
        spec += face.path;
        last_weight = face.weight;
        first = false;
    }
    return first ? std::string("sans-serif") : spec;
}

} // namespace

void ManagedFontCatalog::scan() {
    std::vector<std::string> dirs;for(auto dir:kFontDirs)dirs.emplace_back(dir);
    if(!root_.empty())dirs.push_back(root_);
    scan_directories(dirs);
}
void ManagedFontCatalog::scan_directories(const std::vector<std::string> &dirs) {
    files_.clear();choices_dirty_=true;int seen=0;
    for(const auto &dir:dirs)read_dir(dir.c_str(),files_,&seen);
    scanned_ = true;
    lyra_log(LyraLogInfo, "Lyra", "font scan: %d files", seen);
}

void ManagedFontCatalog::configure(int regular, int light, int bold, int heavy) {
    if (regular <= 0 || light <= 0 || bold <= 0 || heavy <= 0) return;
    if(scanned_ && !choices_dirty_ && regular==regular_ && light==light_ && bold==bold_ && heavy==heavy_){
        spec_=automatic_spec_;if(requested_!="auto" && requested_!="custom")if(auto *selected=choice(requested_))spec_=selected->spec;
        return;
    }
    if (!scanned_) scan();
    regular_=regular;light_=light;bold_=bold;heavy_=heavy;
    Rank best = Rank::None;
    std::vector<Face> chosen;
    int order = 0;
    for (const InstalledFont &file : files_) {
        if(!usable_text_name(file.name) || (!root_.empty() && file.path.compare(0,root_.size()+1,root_+"/")==0))continue;
        Rank rank = Rank::None;
        if (!classify(file.name, &rank)) continue;
        if (rank > best) continue;
        int weight = 0;
        if (!weight_for(file.name, regular, light, bold, heavy, &weight)) continue;
        if (rank < best) {
            best = rank;
            chosen.clear();
        }
        chosen.push_back({file.path, weight, order++});
    }
    if (chosen.empty()) {
        spec_ = "sans-serif";
        family_ = "sans-serif";
        lyra_log(LyraLogInfo, "Lyra", "font catalog: using sans-serif");
    } else {
        spec_ = build_spec(std::move(chosen));
        family_ = family_label(best);
        lyra_log(LyraLogInfo, "Lyra", "font catalog: using %s", family_.c_str());
    }    automatic_spec_=spec_;view_.automatic=family_;
    build_choices(regular,light,bold,heavy);
    if(requested_!="auto" && requested_!="custom")if(auto *selected=choice(requested_))spec_=selected->spec;
}

int ManagedFontCatalog::supply(int64_t, char *out, int cap) const {
    if (out == nullptr || cap <= 0) return 0;
    size_t n = std::min(spec_.size(), size_t(cap - 1));
    std::memcpy(out, spec_.data(), n);
    out[n] = 0;
    return int(n);
}

void ManagedFontCatalog::user_root(std::string root){
    auto path=root.empty()?std::string():root+"/lyra/fonts";
    if(path!=root_){root_=std::move(path);scanned_=false;}
}
void ManagedFontCatalog::preferences(const custom_fonts::Settings &settings){settings_=settings;requested_=settings.family;if(choice_limit_!=size_t(settings.recommended_limit))choices_dirty_=true;choice_limit_=size_t(settings.recommended_limit);}
const custom_fonts::Choice *ManagedFontCatalog::choice(const std::string &id)const{
    for(const auto &item:view_.choices)if(item.id==id)return &item;
    // Old releases persisted a content-addressed filename as the choice ID.
    // Resolve it from the grouped descriptor, without a second alias catalog.
    if(starts_with(id,"user:")){
        auto path="/"+id.substr(std::strlen("user:"));
        for(const auto &item:view_.choices)if(item.user){auto at=item.spec.find(path);
            if(at!=std::string::npos && (at+path.size()==item.spec.size() || item.spec[at+path.size()]=='|'))return &item;
        }
    }
    return nullptr;
}
bool ManagedFontCatalog::remove(const std::string &id){
    if(started_ || root_.empty())return false;
    const auto *target=choice(id);if(!target || !target->user)return false;
    auto family=target->label;
    bool removed=false,ok=true;
    for(const auto &file:files_)if(file.path.compare(0,root_.size()+1,root_+"/")==0){
        int fd=open(file.path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)continue;
        FontMetadata metadata;bool valid=read_font_metadata(fd,&metadata);close(fd);
        if(valid && lower_ascii(metadata.family.c_str())==lower_ascii(family.c_str())){
            if(unlink(file.path.c_str())==0)removed=true;else ok=false;
        }
    }
    if(removed)scan();
    return removed && ok;
}
bool ManagedFontCatalog::clean_family(const std::string &name){
    auto n=lower_ascii(name.c_str());
    for(auto denied:{"hand","script","brush","cursive","pinyin","romaniz","emoji","symbol","dingbat","mono","comic","kaishu","kaiti","xingkai","callig","decor","italic","oblique","rounded","square"})if(contains(n,denied))return false;
    for(auto allowed:{"pingfang","mi sans","misans","harmonyos","noto sans","notosans","source han sans","sourcehansans","roboto","arial","helvetica","sans serif","sans-serif","droid sans"})if(contains(n,allowed))return true;
    return false;
}
void ManagedFontCatalog::build_choices(int regular,int light,int bold,int heavy){
    struct Group {std::string label;std::vector<Face> faces;bool user=false,preferred=false;};
    std::map<std::string,Group> groups;int order=0;
    for(const auto &file:files_){
        bool user=!root_.empty() && file.path.compare(0,root_.size()+1,root_+"/")==0;
        int fd=open(file.path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)continue;
        FontMetadata metadata;bool valid=read_font_metadata(fd,&metadata);close(fd);if(!valid)continue;
        if(!user && (!clean_family(metadata.family+" "+file.name) || contains(file.name,"italic") || contains(file.name,"oblique")))continue;
        std::string id=(user?"user:":"system:")+lower_ascii(metadata.family.c_str());
        auto &group=groups[id];group.label=metadata.family;group.user=user;group.preferred=false;
        auto family_name=lower_ascii(metadata.family.c_str());
        for(size_t at=0;at<settings_.preferred_families.size();){auto end=settings_.preferred_families.find('|',at);auto preferred=settings_.preferred_families.substr(at,end-at);if(!preferred.empty() && (contains(file.name,preferred.c_str()) || contains(family_name,preferred.c_str())))group.preferred=true;if(end==std::string::npos)break;at=end+1;}
        if(metadata.variable)for(int weight:{regular,light,bold,heavy})group.faces.push_back({file.path,weight,order++});
        else {int weight=metadata.weight;if(weight<=0 && !weight_for(file.name,regular,light,bold,heavy,&weight))weight=regular;group.faces.push_back({file.path,weight,order++});}
    }
    std::vector<custom_fonts::Choice> choices;
    for(auto &entry:groups)choices.push_back({entry.first,entry.second.label,build_spec(std::move(entry.second.faces)),entry.second.user,entry.second.preferred});
    std::stable_sort(choices.begin(),choices.end(),[](const auto &a,const auto &b){
        bool ap=a.preferred,bp=b.preferred;
        if(ap!=bp)return ap;
        if(a.user!=b.user)return !a.user;
        return a.label<b.label;
    });
    size_t system_count=0;
    choices.erase(std::remove_if(choices.begin(),choices.end(),[&](const auto &item){return !item.user && ++system_count>choice_limit_;}),choices.end());
    choices_dirty_=false;
    bool same=choices.size()==view_.choices.size();
    for(size_t i=0;same && i<choices.size();++i)same=choices[i].id==view_.choices[i].id && choices[i].spec==view_.choices[i].spec && choices[i].label==view_.choices[i].label;
    if(!same){view_.choices=std::move(choices);++view_.revision;}
}
} // namespace lyra
