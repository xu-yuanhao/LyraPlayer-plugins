#include "font_catalog.h"
#include <algorithm>
#include "font_metadata.h"
#include <array>
#include <chrono>
#include <climits>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
namespace lyra {
struct ManagedFontCatalog::Import {
    ManagedFontCatalog *owner=nullptr;int fd=-1;size_t maximum=0,limit=0;int timeout=0;
    void *user=nullptr;void (*wake)(void*)=nullptr;
};
ManagedFontCatalog::~ManagedFontCatalog(){join();}
void ManagedFontCatalog::join(){if(started_){pthread_join(thread_,nullptr);started_=false;}delete import_;import_=nullptr;}
bool ManagedFontCatalog::import_fd(int fd,const custom_fonts::Settings &settings,void *user,void (*wake)(void*)){
    if(fd<0)return false;
    if(started_ || root_.empty()){close(fd);return false;}
    import_=new Import{this,fd,size_t(settings.import_max_bytes),size_t(settings.import_limit),int(settings.import_timeout_ms),user,wake};
    result_=0;
    if(pthread_create(&thread_,nullptr,import_worker,import_)!=0){close(fd);delete import_;import_=nullptr;return false;}
    started_=true;return true;
}
int ManagedFontCatalog::finish_import(){
    if(!started_)return 0;int result=result_.load(std::memory_order_acquire);if(!result)return 0;
    join();result_=0;if(result>0)scan();return result;
}
void *ManagedFontCatalog::import_worker(void *value){
    auto &job=*static_cast<Import*>(value);auto &self=*job.owner;
    int result=-1,out=-1;std::string temporary=self.root_+"/.incoming";
    auto copy=[&]()->bool{
        auto parent=self.root_.substr(0,self.root_.find_last_of('/'));
        if(mkdir(parent.c_str(),S_IRWXU)!=0 && errno!=EEXIST)return false;
        if(mkdir(self.root_.c_str(),S_IRWXU)!=0 && errno!=EEXIST)return false;
        size_t installed=0;
        if(auto dir=opendir(self.root_.c_str())){while(auto entry=readdir(dir))if(entry->d_name[0]!='.')++installed;closedir(dir);}
        out=open(temporary.c_str(),O_RDWR|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,S_IRUSR|S_IWUSR);if(out<0)return false;
        auto flags=fcntl(job.fd,F_GETFL);if(flags<0 || fcntl(job.fd,F_SETFL,flags|O_NONBLOCK)!=0)return false;
        auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(job.timeout);
        std::array<uint8_t,size_t(std::numeric_limits<uint16_t>::max())+1> buffer{};
        size_t total=0;uint64_t hash=1469598103934665603ull;
        for(;;){
            auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();if(remaining<=0)return false;
            pollfd source{job.fd,POLLIN,0};int ready=poll(&source,1,int(std::min<int64_t>(remaining,INT_MAX)));
            if(ready<0 && errno==EINTR)continue;if(ready<=0 || (source.revents&POLLNVAL))return false;
            auto count=read(job.fd,buffer.data(),buffer.size());
            if(count<0){if(errno==EINTR || errno==EAGAIN)continue;return false;}
            if(!count)break;if(size_t(count)>job.maximum-total)return false;
            for(ssize_t i=0;i<count;++i)hash=(hash^buffer[size_t(i)])*1099511628211ull;
            size_t done=0;while(done<size_t(count)){auto written=write(out,buffer.data()+done,size_t(count)-done);if(written<0 && errno==EINTR)continue;if(written<=0)return false;done+=size_t(written);}
            total+=size_t(count);
        }
        FontMetadata metadata;if(!total || !read_font_metadata(out,&metadata) || fsync(out)!=0)return false;
        char filename[sizeof(uint64_t)*CHAR_BIT]{};std::snprintf(filename,sizeof(filename),"/%016llx-%llu.ttf",(unsigned long long)hash,(unsigned long long)total);
        auto destination=self.root_+filename;
        // Android app-data policy permits rename but can reject hard links.
        // This private directory has a single import worker; inspect an existing
        // content-addressed file before atomically publishing a new one.
        int existing=open(destination.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(existing<0){
            if(errno!=ENOENT)return false;
            if(installed>=job.limit){result=ImportLimit;return false;}
            return rename(temporary.c_str(),destination.c_str())==0;
        }
        struct stat st{};bool identical=fstat(existing,&st)==0 && st.st_size>=0 && uint64_t(st.st_size)==total;
        std::array<uint8_t,std::tuple_size<decltype(buffer)>::value> second{};
        for(size_t offset=0;identical && offset<total;){size_t count=std::min(buffer.size(),total-offset);
            identical=pread(out,buffer.data(),count,off_t(offset))==ssize_t(count) && pread(existing,second.data(),count,off_t(offset))==ssize_t(count) && !std::memcmp(buffer.data(),second.data(),count);offset+=count;}
        close(existing);return identical;
    };
    if(copy())result=1;
    close(job.fd);if(out>=0)close(out);unlink(temporary.c_str());
    self.result_.store(result,std::memory_order_release);
    if(job.wake)job.wake(job.user);return nullptr;
}
}
