// Real FFmpeg decoding and real AAudio. Only zeros reach the device output.
#include "playback/engine.h"
#include "config/toml.h"
#include <cmath>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <unistd.h>
static int64_t ns(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return int64_t(t.tv_sec)*1000000000+t.tv_nsec;}
static void check(bool ok,const char* text){if(!ok){std::fprintf(stderr,"FAIL %s\n",text);std::exit(1);}}
template<class F> static bool until(F fn){for(int i=0;i<2000;++i){if(fn())return true;std::this_thread::sleep_for(std::chrono::milliseconds(2));}return false;}
std::atomic<int> real_opens{0},real_xruns{0},probe_result{0};std::atomic<int64_t> probe_frame{0},probe_written{0},probe_read{0},probe_stamp{0};
static void u16(FILE* f,unsigned v){std::fputc(v&255,f);std::fputc((v>>8)&255,f);}
static void u32(FILE* f,unsigned v){u16(f,v);u16(f,v>>16);}
int main(int argc,char **argv){check(argc>1,"configuration path");const int fixture_rate=argc>3?std::atoi(argv[3]):48000;check(fixture_rate==48000 || fixture_rate==44100,"fixture rate");Engine engine;AudioFeatureSettings features{180,1800,10,1};
        Toml toml;std::string error;check(toml.parse(read_text_file(argv[1]),&error),"transition config fixture");
        AudioTransitionSettings settings;check(toml.require_bool("audio_transition.enabled",&settings.enabled,&error),"transition enabled fixture");
        check(toml.require_number("audio_transition.automatic_ms",&settings.automatic_ms,&error),"transition automatic_ms fixture");
        check(toml.require_number("audio_transition.manual_ms",&settings.manual_ms,&error),"transition manual_ms fixture");

        check(configure_transition(engine.transition_interface(),settings)==0,"configure real engine transition");
        auto drum_fixture=[&](int bpm,int channel){
            const int input_rate=channel?(fixture_rate==48000?44100:48000):fixture_rate;
            char path[96];std::snprintf(path,sizeof(path),"/data/local/tmp/lyra-mix-%d-%d.wav",int(getpid()),channel);
            FILE *f=std::fopen(path,"wb");check(f,"mixed fixture file");const int frames=input_rate*20,bytes=frames*4;
            std::fwrite("RIFF",1,4,f);u32(f,bytes+36);std::fwrite("WAVEfmt ",1,8,f);u32(f,16);u16(f,1);u16(f,2);u32(f,input_rate);u32(f,input_rate*4);u16(f,4);u16(f,16);std::fwrite("data",1,4,f);u32(f,bytes);
            for(int i=0;i<frames;++i){double t=double(i)/input_rate,phase=std::fmod(t+.1,60./bpm);
                int16_t x=int16_t((.6*std::exp(-phase*90)*std::sin(6.283185307179586*70*phase))*32767);
                u16(f,channel?0:uint16_t(x));u16(f,channel?uint16_t(x):0);}
            std::fclose(f);int out=::open(path,O_RDONLY);::unlink(path);return out;
        };

int left=drum_fixture(120,0),right=drum_fixture(124,1);
engine.start(dup(left),101,20000,features,101);
check(until([&]{auto s=engine.snapshot();return s.clock_ready && s.position_us>=2800000;}),"hardware outgoing ready");
engine.start(dup(right),102,20000,features,102);
check(until([&]{auto s=engine.snapshot();return s.generation==102 && s.clock_ready && !s.seeking && s.position_us>650000;}),"hardware matched handoff");
engine.start(dup(left),103,20000,features,103);
check(until([&]{auto s=engine.snapshot();return s.generation==103 && s.clock_ready && !s.seeking && s.position_us>400000;}),"hardware retarget while stretched");
engine.pause();auto position=engine.snapshot().position_us;std::this_thread::sleep_for(std::chrono::milliseconds(90));check(engine.snapshot().position_us==position,"hardware pause freezes");check(engine.resume(),"hardware resumes");
engine.seek(5000,Engine::kProgressSeek);check(until([&]{auto s=engine.snapshot();return !s.seeking && s.clock_ready && s.position_us>=5000000;}),"hardware seek lands");
if(argc>2){int compressed=::open(argv[2],O_RDONLY);check(compressed>=0,"hardware codec fixture");engine.start(compressed,104,4000,features,104);check(until([&]{auto s=engine.snapshot();return s.generation==104 && !s.seeking && s.clock_ready && s.position_us>700000;}),"hardware mixed rate joins");
engine.start(dup(right),105,20000,features,105);check(until([&]{auto s=engine.snapshot();return s.generation==105 && !s.seeking && s.clock_ready && s.position_us>1000000;}),"hardware compressed outgoing released");}
for(int round=0;round<60;++round){
    int old_opens=real_opens.load();
    engine.seek(1000+(round%3)*3000,Engine::kProgressSeek);
    bool ready=until([&]{auto s=engine.snapshot();return !s.seeking && s.clock_ready && s.position_us>=int64_t(1000+(round%3)*3000)*1000;});
    if(!ready){auto s=engine.snapshot();std::printf("clock failure round=%d pos=%lld ready=%d seeking=%d output=%lld rawts=%lld written=%lld read=%lld stamp=%lld now=%lld result=%d\n",round,(long long)s.position_us,s.clock_ready,s.seeking,(long long)s.output_frame,(long long)probe_frame.load(),(long long)probe_written.load(),(long long)probe_read.load(),(long long)probe_stamp.load(),(long long)ns(),probe_result.load());return 2;}
    check(engine.snapshot().sample_rate==fixture_rate,"mixed-source seek preserves actual output rate");
    check(real_opens==old_opens+1,"seek opens one fresh output epoch");
    engine.prime_start(0,false);engine.start(dup(round%2?left:right),500+round,20000,features,500+round);
    check(until([&]{auto s=engine.snapshot();return s.clock_ready && !s.seeking && s.position_us>100000;}),"loop handoff ready");
    check(real_opens==old_opens+1,"track handoff reuses the seek output stream");
}
engine.log_underruns();engine.stop();::close(left);::close(right);
std::printf("60 seek/handoff rounds passed: output_opens=%d max_xruns=%d\n",real_opens.load(),real_xruns.load());check(real_xruns==0,"no hardware underruns in transition test");}
