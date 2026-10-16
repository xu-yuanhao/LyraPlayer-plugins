// Real Android extractor/codec with a silent, measurable output boundary.
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
static std::mutex barrier_mutex;
static std::condition_variable barrier_cv;
static bool hold_flush=false,flush_entered=false;
static std::atomic<bool> mapped{false};
static std::atomic<int64_t> pcm_delta{0};
static std::atomic<int> output_opens{0},mixed_frames{0};
static std::atomic<bool> watch_mix{false},watch_pitch{false};
static std::atomic<int> pitch_frames{0},pitch_crossings{0};
static bool pitch_positive=false;
namespace lyra::audio {
struct Stream { std::mutex mutex;State state=Open;int rate=48000;int64_t written=0,anchor=0,when=0,start_written=0; };
static int64_t played(Stream* s){return s->state==Started?std::min(s->written,s->anchor+(ns()-s->when)*s->rate/1000000000):s->anchor;}
Stream* open(int rate,const AudioOutputSettings&,ErrorCallback,void*){++output_opens;auto* s=new Stream;s->rate=rate;return s;}
Result close(Stream* s){
    {std::unique_lock<std::mutex> lock(barrier_mutex);if(hold_flush){flush_entered=true;barrier_cv.notify_all();barrier_cv.wait(lock,[]{return !hold_flush;});}}
    delete s;return Ok;
}
int sample_rate(Stream* s){return s->rate;}
int xruns(Stream*){return 0;}int capacity(Stream*){return 2048;}int size(Stream*){return 2048;}int burst(Stream*){return 256;}int set_size(Stream*,int n){return n;}
int64_t written(Stream* s){std::lock_guard<std::mutex> lock(s->mutex);return s->written;}
State state(Stream* s){std::lock_guard<std::mutex> lock(s->mutex);return s->state;}
Result wait_state(Stream* s,State,State* next,int64_t){*next=state(s);return Ok;}
Result start(Stream* s){std::lock_guard<std::mutex> lock(s->mutex);s->when=ns();s->start_written=s->written;s->state=Started;mapped=false;return Ok;}
Result pause(Stream* s){std::lock_guard<std::mutex> lock(s->mutex);s->anchor=played(s);s->state=Paused;return Ok;}
Result stop(Stream* s){std::lock_guard<std::mutex> lock(s->mutex);s->state=Stopped;return Ok;}
Result flush(Stream* s){
    {std::unique_lock<std::mutex> lock(barrier_mutex);if(hold_flush){flush_entered=true;barrier_cv.notify_all();barrier_cv.wait(lock,[]{return !hold_flush;});}}
    std::lock_guard<std::mutex> lock(s->mutex);
    // Model counters advancing when old queued audio is discarded.
    s->written+=256;s->anchor=s->written;s->state=Flushed;mapped=false;return Ok;
}
Result write(Stream* s,const void* bytes,int count,int64_t){
    std::unique_lock<std::mutex> lock(s->mutex);
    if(s->state!=Started)return ErrorInvalidState;
    int room=int(std::max<int64_t>(0,2048-(s->written-played(s))));
    int n=std::min({count,room,256});
    if(!n){lock.unlock();std::this_thread::sleep_for(std::chrono::milliseconds(1));return 0;}
    if(!mapped && s->written-s->start_written>=1024){
        auto* pcm=static_cast<const int16_t*>(bytes);
        int64_t source=int64_t(pcm[1])*30000+pcm[0]+15000;
        pcm_delta=source-s->written;mapped=true;
    }
    if(watch_mix){auto *pcm=static_cast<const int16_t*>(bytes);for(int i=0;i<n;++i)if(std::abs(int(pcm[i*2]))>100 && std::abs(int(pcm[i*2+1]))>100)++mixed_frames;}
    if(watch_pitch){auto *pcm=static_cast<const int16_t*>(bytes);for(int i=0;i<n;++i){bool positive=pcm[i*2]>=0;if(positive && !pitch_positive)++pitch_crossings;pitch_positive=positive;}pitch_frames+=n;}
    s->written+=n;return n;
}
Result timestamp(Stream* s,int64_t* frame,int64_t* stamp){std::lock_guard<std::mutex> lock(s->mutex);if(s->state!=Started)return ErrorInvalidState;*frame=played(s);*stamp=ns();return Ok;}
}
static void u16(FILE* f,unsigned v){std::fputc(v&255,f);std::fputc((v>>8)&255,f);}
static void u32(FILE* f,unsigned v){u16(f,v);u16(f,v>>16);}
static int fixture(){
    char path[96];std::snprintf(path,sizeof(path),"/data/local/tmp/lyra-seek-%d.wav",int(getpid()));
    FILE* f=std::fopen(path,"wb");check(f,"fixture file");
    constexpr int frames=48000*20,bytes=frames*4;
    std::fwrite("RIFF",1,4,f);u32(f,bytes+36);std::fwrite("WAVEfmt ",1,8,f);u32(f,16);u16(f,1);u16(f,2);u32(f,48000);u32(f,192000);u16(f,4);u16(f,16);std::fwrite("data",1,4,f);u32(f,bytes);
    for(int i=0;i<frames;++i){u16(f,unsigned(int16_t(i%30000-15000)));u16(f,unsigned(int16_t(i/30000)));}
    std::fclose(f);int fd=::open(path,O_RDONLY);::unlink(path);return fd;
}
#include "playback/edge_fixture_test.h"
#include "playback/adaptive_fixture_test.h"
#include "playback/full_fixture_test.h"
void test_adaptive_runtime(const AudioTransitionSettings &);
void test_stem_runtime(const AudioTransitionSettings &);
void test_tagged_flac(const char *);
static void aligned(Engine& engine,int64_t target){
    check(until([&]{auto s=engine.snapshot();return !s.seeking && s.clock_ready && mapped && s.position_us>=target*1000;}),"latest seek reaches audio output");
    auto s=engine.snapshot();int64_t audio=(s.output_frame+pcm_delta.load())*1000000/s.sample_rate;
    check(std::llabs(s.position_us-audio)<=1000,"clock matches samples actually submitted, not requested position");
    check(s.position_us>=target*1000 && s.position_us<target*1000+500000,"seek lands near requested media position");
}
int main(int argc,char **argv){
    int fd=fixture();check(fd>=0,"fixture descriptor");
    Engine engine;AudioFeatureSettings features{180,1800,10,1};
    engine.start(dup(fd),7,20000,features,1);aligned(engine,0);
    auto initial_revision=engine.snapshot().revision;
    {std::lock_guard<std::mutex> lock(barrier_mutex);hold_flush=true;flush_entered=false;}
    engine.seek(4000,Engine::kLyricSeek);
    check(until([]{std::lock_guard<std::mutex> lock(barrier_mutex);return flush_entered;}),"decoder enters controlled flush");
    std::atomic<bool> posted{false};
    std::thread latest([&]{engine.seek(9000,Engine::kLyricSeek);posted=true;});
    check(until([&]{return posted.load();}),"new lyric seek posts while prior decoder flush is blocked");
    auto waiting=engine.snapshot();check(waiting.seeking && waiting.seek_serial==2,"new target has its own serial before old seek completes");
    {std::lock_guard<std::mutex> lock(barrier_mutex);hold_flush=false;}barrier_cv.notify_all();latest.join();
    aligned(engine,9000);auto landed=engine.snapshot();
    check(landed.seek_serial==landed.landed_seek && landed.revision==initial_revision+1,"superseded seek never publishes an intermediate clock revision");
    engine.pause();engine.seek(2500,Engine::kProgressSeek);
    check(until([&]{auto s=engine.snapshot();return !s.seeking && s.paused && s.position_us==2500000;}),"paused seek lands and remains frozen");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));check(engine.snapshot().position_us==2500000,"paused landed position does not drift");
    check(engine.resume(),"resume after paused seek");aligned(engine,2500);
    for(int i=0;i<80;++i)engine.seek(i%2?6750:1250,i%2?Engine::kLyricSeek:Engine::kProgressSeek);
    aligned(engine,6750);check(engine.snapshot().seek_serial==engine.snapshot().landed_seek,"continuous clicks settle on last request");
    engine.seek(19900,Engine::kProgressSeek);aligned(engine,19900);
    engine.seek(1000,Engine::kLyricSeek);aligned(engine,1000);
    engine.stop();engine.start(dup(fd),8,20000,features,2);aligned(engine,0);
    check(engine.snapshot().generation==2 && engine.snapshot().track_id==8,"new track cannot inherit previous seek timeline");
    engine.stop();engine.prime_start(5000,true);engine.start(dup(fd),9,20000,features,3);
    check(engine.snapshot().position_us==5000000,"primed restoration publishes its frozen anchor before decoder output");
    check(until([&]{auto s=engine.snapshot();return s.landed_seek==s.seek_serial && !s.seeking && s.paused;}),"paused restoration lands");
    check(engine.resume(),"restored paused track resumes");aligned(engine,5000);
    engine.stop();
    if(argc>1){
        Toml toml;std::string error;check(toml.parse(read_text_file(argv[1]),&error),"transition config fixture");
        AudioTransitionSettings settings;check(toml.require_bool("audio_transition.enabled",&settings.enabled,&error),"transition enabled fixture");
        check(toml.require_number("audio_transition.automatic_ms",&settings.automatic_ms,&error),"transition automatic_ms fixture");
        check(toml.require_number("audio_transition.manual_ms",&settings.manual_ms,&error),"transition manual_ms fixture");
        check(toml.require_number("audio_transition.bridge_ms",&settings.bridge_ms,&error),"transition bridge_ms fixture");
        check(toml.require_number("audio_transition.analysis_ms",&settings.analysis_ms,&error),"transition analysis_ms fixture");
        check(toml.require_number("audio_transition.confidence",&settings.confidence,&error),"transition confidence fixture");
        check(toml.require_number("audio_transition.max_rate_delta",&settings.max_rate_delta,&error),"transition max_rate_delta fixture");
        check(toml.require_number("audio_transition.bass_hz",&settings.bass_hz,&error),"transition bass_hz fixture");
        check(toml.require_number("audio_transition.return_ms",&settings.return_ms,&error),"transition return_ms fixture");
        check(toml.require_number("audio_transition.feature_hz",&settings.feature_hz,&error),"transition feature_hz fixture");
        check(toml.require_number("audio_transition.bpm_min",&settings.bpm_min,&error),"transition bpm_min fixture");
        check(toml.require_number("audio_transition.bpm_max",&settings.bpm_max,&error),"transition bpm_max fixture");
        check(toml.require_number("audio_transition.onset_floor",&settings.onset_floor,&error),"transition onset_floor fixture");
        check(toml.require_number("audio_transition.peak_factor",&settings.peak_factor,&error),"transition peak_factor fixture");
        check(toml.require_number("audio_transition.minimum_onsets",&settings.minimum_onsets,&error),"transition minimum_onsets fixture");
        check(toml.require_number("audio_transition.confident_onsets",&settings.confident_onsets,&error),"transition confident_onsets fixture");
        check(toml.require_number("audio_transition.interval_penalty",&settings.interval_penalty,&error),"transition interval_penalty fixture");
        check(toml.require_number("audio_transition.bass_start",&settings.bass_start,&error),"transition bass_start fixture");
        check(toml.require_number("audio_transition.bass_end",&settings.bass_end,&error),"transition bass_end fixture");
        check(toml.require_number("audio_transition.limiter_release_ms",&settings.limiter_release_ms,&error),"transition limiter_release_ms fixture");
        check(toml.require_number("audio_transition.peak_limit",&settings.peak_limit,&error),"transition peak_limit fixture");
        check(toml.require_number("audio_transition.maximum_track_fraction",&settings.maximum_track_fraction,&error),"transition maximum_track_fraction fixture");
        check(toml.require_number("audio_transition.beat_group",&settings.beat_group,&error),"transition beat_group fixture");

        load_adaptive_settings(toml,settings);
        check(configure_transition(engine.transition_interface(),settings)==0,"configure real engine transition");
        auto drum_fixture=[&](int bpm,int channel){
            char path[96];std::snprintf(path,sizeof(path),"/data/local/tmp/lyra-mix-%d-%d.wav",int(getpid()),channel);
            FILE *f=std::fopen(path,"wb");check(f,"mixed fixture file");constexpr int frames=48000*20,bytes=frames*4;
            std::fwrite("RIFF",1,4,f);u32(f,bytes+36);std::fwrite("WAVEfmt ",1,8,f);u32(f,16);u16(f,1);u16(f,2);u32(f,48000);u32(f,192000);u16(f,4);u16(f,16);std::fwrite("data",1,4,f);u32(f,bytes);
            for(int i=0;i<frames;++i){double t=double(i)/48000,phase=bpm>0?std::fmod(t+.1,60./bpm):0;
                int16_t x=bpm>0?int16_t((.6*std::exp(-phase*90)*std::sin(6.283185307179586*70*phase))*32767):6000;
                u16(f,channel?0:uint16_t(x));u16(f,channel?uint16_t(x):0);}
            std::fclose(f);int out=::open(path,O_RDONLY);::unlink(path);return out;
        };
        int left=drum_fixture(120,0),right=drum_fixture(124,1);check(left>=0 && right>=0,"mixing fixture descriptors");
        engine.start(dup(left),20,20000,features,20);
        check(until([&]{return engine.snapshot().clock_ready && engine.snapshot().position_us>=2800000;}),"outgoing beat context ready");
        int streams=output_opens.load();mixed_frames=0;watch_mix=true;
        engine.start(dup(right),21,20000,features,21);
        {auto pending=engine.snapshot();check(!pending.seeking || pending.position_us==0,"prepared target never inherits old audible position");}
        check(until([&]{auto s=engine.snapshot();return s.generation==21 && !s.seeking && s.position_us>150000;}),"manual transition reaches incoming PCM");
        check(output_opens.load()==streams,"handoff shares one output stream");
        engine.pause();auto paused=engine.snapshot().position_us;
        std::this_thread::sleep_for(std::chrono::milliseconds(80));check(engine.snapshot().position_us==paused,"transition pause freezes hardware clock");
        check(engine.resume(),"resume mixed audio");
        check(until([&]{return mixed_frames.load()>64;}),"two channel-isolated sources actually overlap");
        auto rate_begin=engine.snapshot();
        check(until([&]{return engine.snapshot().position_us>=rate_begin.position_us+300000;}),"beat-locked audio advances");
        auto rate_end=engine.snapshot();
        double media_ratio=double(rate_end.position_us-rate_begin.position_us)*rate_end.sample_rate/(double(rate_end.output_frame-rate_begin.output_frame)*1000000);
        std::printf("beat-locked audible media/output ratio=%g\n",media_ratio);
        check(media_ratio>.94 && media_ratio<.99,"matched transition uses actual non-unit source clock rate");
        engine.start(dup(left),22,20000,features,22);
        engine.seek(7500,Engine::kProgressSeek);
        check(until([&]{auto s=engine.snapshot();return s.generation==22 && !s.seeking && s.landed_seek==s.seek_serial && s.position_us>=7500000;}),"seek posted during preparation cancels mixer and lands on incoming track");
        engine.start(dup(left),23,20000,features,23);engine.start(dup(right),24,20000,features,24);
        check(until([&]{auto s=engine.snapshot();return s.generation==24 && s.track_id==24 && !s.seeking && s.clock_ready;}),"rapid handoff retains latest target");
        watch_mix=false;
        if(argc>2){int compressed=::open(argv[2],O_RDONLY);check(compressed>=0,"FLAC descriptor");
            engine.start(compressed,25,4000,features,25);
            check(until([&]{auto s=engine.snapshot();return s.generation==25 && !s.seeking && s.clock_ready && s.position_us>100000;}),"44.1k MediaCodec joins 48k stream");
            check(engine.snapshot().sample_rate==48000,"incoming rate conversion preserves shared output");
            engine.seek(1500,Engine::kProgressSeek);
            check(until([&]{auto v=engine.snapshot();return !v.seeking && v.clock_ready && v.position_us>=1500000;}),"mixed-rate seek lands");
            check(engine.snapshot().sample_rate==48000,"mixed-rate seek preserves PCM output rate");
            pitch_frames=0;pitch_crossings=0;watch_pitch=true;
            auto seek_begin=engine.snapshot();
            check(until([&]{return engine.snapshot().position_us>seek_begin.position_us+500000;}),"mixed-rate seek playback advances");
            watch_pitch=false;double pitch_hz=double(pitch_crossings.load())*48000/pitch_frames.load();
            std::printf("mixed-rate post-seek measured pitch=%g Hz\n",pitch_hz);check(std::abs(pitch_hz-440)<5,"seek preserves decoded tone pitch");
            auto seek_end=engine.snapshot();double seek_ratio=double(seek_end.position_us-seek_begin.position_us)*seek_end.sample_rate/(double(seek_end.output_frame-seek_begin.output_frame)*1000000);
            std::printf("mixed-rate post-seek media/output ratio=%g\n",seek_ratio);
            check(std::abs(seek_ratio-1)<.005,"seek resets tempo and source timeline to unity");
            engine.seek(3950,Engine::kProgressSeek);
            check(until([&]{auto s=engine.snapshot();return !s.seeking && s.clock_ready && s.position_us>=3950000;}),"near EOF seek reaches compressed output");
            engine.start(dup(right),26,20000,features,26);
            check(until([&]{auto s=engine.snapshot();return s.generation==26 && s.active && !s.seeking && s.clock_ready;}),"EOF admission never strands a new target");
        }
        engine.stop();
        int flat_left=drum_fixture(0,0),flat_right=drum_fixture(0,1);
        engine.start(flat_left,27,20000,features,27);
        check(until([&]{return engine.snapshot().clock_ready && engine.snapshot().position_us>200000;}),"non-rhythmic outgoing ready");
        watch_mix=true;mixed_frames=0;
        engine.start(flat_right,28,20000,features,28);
        check(until([&]{return engine.snapshot().generation==28 && !engine.snapshot().seeking && engine.snapshot().position_us>int64_t(settings.manual_ms*350);}),"non-rhythmic handover advances");
        int early_overlap=mixed_frames.load();
        check(until([&]{return engine.snapshot().position_us>int64_t(settings.manual_ms*650);}),"non-rhythmic handover continues");
        check(mixed_frames.load()>early_overlap+4800,"low-confidence audio still overlaps well beyond 320ms");
        watch_mix=false;engine.stop();::close(left);::close(right);
        std::puts("Android real decoder transitions: overlap, shared stream, pause, seek, rapid supersession and mixed sample rates passed");
        test_transition_edges(engine,settings,features,toml);
        load_adaptive_settings(toml,settings);test_adaptive_runtime(settings);load_full_settings(toml,settings);test_stem_runtime(settings);if(argc>2)test_tagged_flac(argv[2]);
    }
    engine.stop();::close(fd);std::puts("Android decoder / PCM clock / rapid lyric and progress seeks passed");
}
