#include "font_metadata.h"
#include <array>
#include <climits>
#include <cstddef>
#include <cstring>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
namespace {
// OpenType offset table, table directory, TTC header and name records.
struct OffsetTable {uint32_t scaler;uint16_t tables,range,selector,shift;};
struct Table {uint32_t tag,checksum,offset,length;};
struct Collection {uint32_t tag,version,count,first;};
struct NameHeader {uint16_t format,count,strings;};
struct NameRecord {uint16_t platform,encoding,language,id,length,offset;};
struct Os2Prefix {uint16_t version,average,weight,width;};
constexpr uint16_t kTypographicFamily=16,kUnicodeWindows=3;
// OpenType OS/2 usWeightClass is defined in the inclusive range 1..1000.
constexpr uint16_t kMinimumWeight=1,kMaximumWeight=1000;
constexpr size_t kMaxTables=256,kMaxNames=512,kMaxNameUnits=128;
constexpr uint32_t kSurrogateStart=0xd800,kSurrogateEnd=0xdfff,kLowSurrogate=0xdc00,kScalarBase=0x10000;
constexpr unsigned kSurrogateBits=10,kUtf8Bits=6;
constexpr uint32_t kTwoUtf8=0x80,kThreeUtf8=0x800,kFourUtf8=0x10000,kUtf8Tail=0x80,kUtf8Mask=0x3f,kUtf8Two=0xc0,kUtf8Three=0xe0,kUtf8Four=0xf0;
uint16_t be16(const uint8_t *p){return uint16_t(uint16_t(p[0])<<CHAR_BIT | p[1]);}
uint32_t be32(const uint8_t *p){uint32_t value=0;for(size_t i=0;i<sizeof(value);++i)value=(value<<CHAR_BIT)|p[i];return value;}
bool bytes(int fd,uint64_t size,uint64_t offset,void *out,size_t count){return offset<=size && count<=size-offset && pread(fd,out,count,off_t(offset))==ssize_t(count);}
void utf8(std::string &s,uint32_t c){
    if(c<kTwoUtf8)s+=char(c);
    else {if(c<kThreeUtf8)s+=char(kUtf8Two|(c>>kUtf8Bits));
        else {if(c<kFourUtf8)s+=char(kUtf8Three|(c>>(kUtf8Bits+kUtf8Bits)));
            else {s+=char(kUtf8Four|(c>>(kUtf8Bits+kUtf8Bits+kUtf8Bits)));s+=char(kUtf8Tail|((c>>(kUtf8Bits+kUtf8Bits))&kUtf8Mask));}
            s+=char(kUtf8Tail|((c>>kUtf8Bits)&kUtf8Mask));}
        s+=char(kUtf8Tail|(c&kUtf8Mask));}
}
std::string decode(const std::vector<uint8_t> &data,bool unicode){
    std::string s;
    if(!unicode){for(auto c:data)if(c>=32 && c<127)s+=char(c);return s;}
    if(data.size()%sizeof(uint16_t))return {};
    for(size_t i=0;i<data.size();i+=sizeof(uint16_t)){
        uint32_t c=be16(data.data()+i);
        if(c>=kSurrogateStart && c<kLowSurrogate){
            i+=sizeof(uint16_t);if(i>=data.size())return {};
            auto low=be16(data.data()+i);if(low<kLowSurrogate || low>kSurrogateEnd)return {};
            c=kScalarBase+((c-kSurrogateStart)<<kSurrogateBits)+(low-kLowSurrogate);
        }else if(c>=kLowSurrogate && c<=kSurrogateEnd)return {};
        if(c<' ' || c=='|' || c=='#' || c=='\t' || c=='\n')return {};
        utf8(s,c);
    }
    return s;
}
}
bool read_font_metadata(int fd,FontMetadata *out){
    if(!out)return false;*out={};struct stat st{};if(fstat(fd,&st)!=0 || st.st_size<=0)return false;
    uint64_t size=uint64_t(st.st_size),base=0;std::array<uint8_t,sizeof(Collection)> prefix{};
    if(!bytes(fd,size,0,prefix.data(),prefix.size()))return false;
    if(!std::memcmp(prefix.data(),"ttcf",sizeof(uint32_t))){if(!be32(prefix.data()+offsetof(Collection,count)))return false;base=be32(prefix.data()+offsetof(Collection,first));}
    std::array<uint8_t,sizeof(OffsetTable)> header{};
    if(!bytes(fd,size,base,header.data(),header.size()))return false;
    bool scaler=be32(header.data())==kScalarBase || !std::memcmp(header.data(),"OTTO",sizeof(uint32_t));
    auto count=be16(header.data()+offsetof(OffsetTable,tables));if(!scaler || !count || count>kMaxTables)return false;
    uint64_t name=0,name_length=0;bool cmap=false,head=false;
    for(size_t i=0;i<count;++i){
        std::array<uint8_t,sizeof(Table)> table{};
        if(!bytes(fd,size,base+sizeof(OffsetTable)+i*sizeof(Table),table.data(),table.size()))return false;
        uint64_t offset=be32(table.data()+offsetof(Table,offset)),length=be32(table.data()+offsetof(Table,length));
        if(offset>size || length>size-offset)return false;
        if(!std::memcmp(table.data(),"name",sizeof(uint32_t))){name=offset;name_length=length;}
        if(!std::memcmp(table.data(),"cmap",sizeof(uint32_t)))cmap=length>=sizeof(uint32_t);
        if(!std::memcmp(table.data(),"head",sizeof(uint32_t)))head=length>=sizeof(OffsetTable);
        if(!std::memcmp(table.data(),"fvar",sizeof(uint32_t)))out->variable=true;
        if(!std::memcmp(table.data(),"OS/2",sizeof(uint32_t)) && length>=sizeof(Os2Prefix)){
            std::array<uint8_t,sizeof(Os2Prefix)> os{};if(!bytes(fd,size,offset,os.data(),os.size()))return false;
            out->weight=be16(os.data()+offsetof(Os2Prefix,weight));
            if(out->weight<kMinimumWeight || out->weight>kMaximumWeight)return false;
        }
    }
    if(!cmap || !head || name_length<sizeof(NameHeader))return false;
    std::array<uint8_t,sizeof(NameHeader)> names{};
    if(!bytes(fd,size,name,names.data(),names.size()))return false;
    auto entries=be16(names.data()+offsetof(NameHeader,count)),strings=be16(names.data()+offsetof(NameHeader,strings));
    if(!entries || entries>kMaxNames || strings>name_length || uint64_t(entries)*sizeof(NameRecord)>name_length-sizeof(NameHeader))return false;
    int priority=-1;
    for(size_t i=0;i<entries;++i){
        std::array<uint8_t,sizeof(NameRecord)> row{};
        if(!bytes(fd,size,name+sizeof(NameHeader)+i*sizeof(NameRecord),row.data(),row.size()))return false;
        auto id=be16(row.data()+offsetof(NameRecord,id));if(id!=1 && id!=kTypographicFamily)continue;
        auto platform=be16(row.data()+offsetof(NameRecord,platform));if(platform!=0 && platform!=1 && platform!=kUnicodeWindows)continue;
        auto length=be16(row.data()+offsetof(NameRecord,length)),offset=be16(row.data()+offsetof(NameRecord,offset));
        if(!length || length>kMaxNameUnits*sizeof(uint16_t) || uint64_t(strings)+offset+length>name_length)continue;
        enum NamePriority {MacFamily,WindowsFamily,TypographicFamily,WindowsTypographicFamily};
        int rank=id==kTypographicFamily?(platform==kUnicodeWindows?WindowsTypographicFamily:TypographicFamily):(platform==kUnicodeWindows?WindowsFamily:MacFamily);
        if(rank<=priority)continue;
        std::vector<uint8_t> data(length);if(!bytes(fd,size,name+strings+offset,data.data(),data.size()))return false;
        auto family=decode(data,platform!=1);if(!family.empty()){out->family=std::move(family);priority=rank;}
    }
    return !out->family.empty();
}
