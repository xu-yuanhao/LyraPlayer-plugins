// Exercise exactly the private decoder used by the plugin, without audio output.
#include <fcntl.h>
#include "../native/playback/engine.cpp"
#include <cstdio>
int main(int argc,char **argv){
    bool expect_empty=argc>1 && !std::strcmp(argv[1],"--expect-empty");
    if(argc<=1+int(expect_empty))return 2;
    for(int i=1+int(expect_empty);i<argc;++i){
        SystemDecode d;d.fd=::open(argv[i],O_RDONLY);
        if(!d.open()){std::printf("FAIL open file %d\n",i);return 1;}
        std::atomic<bool> cancel{false};int16_t pcm[512];int got;int64_t samples=0;
        while((got=d.pull(pcm,256,48000,cancel))>0)samples+=got;
        if(got<0 || (expect_empty?samples!=0:samples==0)){std::printf("FAIL decoded samples %lld rc %d\n",(long long)samples,got);return 1;}
        if(!expect_empty){
            for(int64_t target:{int64_t(0),d.duration_us/2,d.duration_us>100000?d.duration_us-100000:0}){
                if(!d.seek(target) || d.pull(pcm,256,48000,cancel)<=0){std::printf("FAIL seek %lld codec %s\n",(long long)target,d.ctx->codec->name);return 1;}
            }
        }
        std::printf("PASS codec=%s decoded_frames=%lld duration_us=%lld empty=%d\n",d.ctx->codec->name,(long long)samples,(long long)d.duration_us,int(expect_empty));
    }
}
