#include "platform/log.h"
#include "playback/clock_math.h"
#include "playback/engine.h"
#include "playback/media_metadata.h"
#include "playback/container_probe.h"
#include <condition_variable>
#include <memory>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
}
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
namespace {
void arm_log() { av_log_set_level(AV_LOG_ERROR); }
void log_fail(const char *what, int code) {
    lyra_log(LyraLogError, "Lyra", "ffmpeg play fail %s %d", what, code);
}
int64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
int64_t now_ms() { return now_ns() / 1000000; }

bool settle_stream(lyra::audio::Stream *stream) {
    for (int i = 0; i < 4; ++i) {
        auto state = lyra::audio::state(stream);
        if (state != lyra::audio::Starting && state != lyra::audio::Pausing &&
            state != lyra::audio::Flushing && state != lyra::audio::Stopping)
            return state != lyra::audio::Disconnected && state != lyra::audio::Closed;
        lyra::audio::State next = state;
        lyra::audio::wait_state(stream, state, &next, 50000000LL);
    }
    return false;
}

bool key_has(const char* key, const char* needle) {
    if (key == nullptr || needle == nullptr) return false;
    for (const char* p = key; *p != '\0'; ++p) {
        const char* a = p;
        const char* b = needle;
        while (*a != '\0' && *b != '\0') {
            char c = *a;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != *b) break;
            ++a;
            ++b;
        }
        if (*b == '\0') return true;
    }
    return false;
}

bool lyric_metadata_key(const char* key) {
    return key_has(key, "lyric") || key_has(key, "uslt") || key_has(key, "sylt");
}

void consider_lyric_dict(AVDictionary* dict, std::string* best, bool* best_synced, const char** best_key) {
    if (dict == nullptr) return;
    const AVDictionaryEntry* entry = nullptr;
    while ((entry = av_dict_get(dict, "", entry, AV_DICT_IGNORE_SUFFIX)) != nullptr) {
        if (!lyric_metadata_key(entry->key) || entry->value == nullptr || entry->value[0] == '\0') continue;
        size_t bytes = std::strlen(entry->value);
        if (bytes > 4u * 1024u * 1024u) continue;
        bool synced = std::strchr(entry->value, '[') != nullptr || std::strstr(entry->value, "<tt") != nullptr;
        bool better = best->empty() || (synced && !*best_synced) || (synced == *best_synced && bytes > best->size());
        if (!better) continue;
        best->assign(entry->value, bytes);
        *best_synced = synced;
        *best_key = entry->key;
    }
}

std::string lyrics_from_context(AVFormatContext* fmt) {
    std::string best;
    bool synced = false;
    const char* key = "";
    if (fmt == nullptr) return best;
    consider_lyric_dict(fmt->metadata, &best, &synced, &key);
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i] != nullptr) consider_lyric_dict(fmt->streams[i]->metadata, &best, &synced, &key);
    }
    if (best.empty()) {
        lyra_log(LyraLogInfo, "Lyra", "lyrics metadata none");
    } else {
        lyra_log(LyraLogInfo, "Lyra", "lyrics metadata key=%s bytes=%zu synced=%d", key, best.size(),
                            synced ? 1 : 0);
    }
    return best;
}

struct SystemDecode {
    int fd=-1,sample_rate=48000,index=-1;
    int64_t duration_us=0,audible_end_us=0,first_media_us=-1,total_frames=0,delivered_frames=0,io_position=0,last_block_us=0;
    bool input_end=false,output_end=false;
    AVIOContext *avio=nullptr;AVFormatContext *fmt=nullptr;AVCodecContext *ctx=nullptr;
    AVPacket *pkt=nullptr;AVFrame *frame=nullptr;SwrContext *swr=nullptr;
    int swr_rate=0,swr_format=-1,swr_output=0;AVChannelLayout swr_layout{};
    size_t stereo_pos=0,prepared_pos=0;std::vector<int16_t> prepared,stereo;
    ~SystemDecode(){
        if(ctx)avcodec_free_context(&ctx);if(frame)av_frame_free(&frame);if(pkt)av_packet_free(&pkt);
        if(fmt)avformat_close_input(&fmt);if(avio){av_freep(&avio->buffer);avio_context_free(&avio);}
        if(swr)swr_free(&swr);av_channel_layout_uninit(&swr_layout);if(fd>=0)::close(fd);
    }
    static int read_io(void *opaque,uint8_t *bytes,int count){
        auto &s=*static_cast<SystemDecode*>(opaque);
        for(;;){ssize_t n=pread(s.fd,bytes,size_t(count),s.io_position);if(n>0){s.io_position+=n;return int(n);}if(!n)return AVERROR_EOF;if(errno!=EINTR)return AVERROR(errno);}
    }
    static int64_t seek_io(void *opaque,int64_t offset,int whence){
        auto &s=*static_cast<SystemDecode*>(opaque);struct stat st{};if(fstat(s.fd,&st))return AVERROR(errno);
        if(whence&AVSEEK_SIZE)return st.st_size;
        int base=whence&~AVSEEK_FORCE;int64_t at=base==SEEK_SET?0:base==SEEK_CUR?s.io_position:base==SEEK_END?st.st_size:-1;
        if(at<0 || (offset>0 && at>INT64_MAX-offset) || (offset<0 && offset < -at))return AVERROR(EINVAL);
        s.io_position=at+offset;return s.io_position;
    }
    bool open_format(){
        auto *buffer=static_cast<uint8_t*>(av_malloc(32768));if(!buffer)return false;
        avio=avio_alloc_context(buffer,32768,0,this,read_io,nullptr,seek_io);if(!avio){av_free(buffer);return false;}
        fmt=avformat_alloc_context();if(!fmt)return false;fmt->pb=avio;fmt->flags|=AVFMT_FLAG_CUSTOM_IO;
        fmt->probesize=96*1024;fmt->format_probesize=96*1024;
        struct stat st{};const AVInputFormat *format=nullptr;
        if(!fstat(fd,&st) && lyra::audio::flac_payload_offset(fd,st.st_size)>=0)format=av_find_input_format("flac");
        return avformat_open_input(&fmt,nullptr,format,nullptr)>=0;
    }
    bool open(){
        if(!open_format() || avformat_find_stream_info(fmt,nullptr)<0)return false;
        index=av_find_best_stream(fmt,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0);if(index<0)return false;
        auto *stream=fmt->streams[index];auto *codec=avcodec_find_decoder(stream->codecpar->codec_id);if(!codec)return false;
        ctx=avcodec_alloc_context3(codec);if(!ctx || avcodec_parameters_to_context(ctx,stream->codecpar)<0)return false;
        ctx->pkt_timebase=stream->time_base;ctx->thread_count=1;
        if(avcodec_open2(ctx,codec,nullptr)<0)return false;
        sample_rate=ctx->sample_rate;if(sample_rate<8000 || sample_rate>384000)return false;
        if(stream->duration!=AV_NOPTS_VALUE)duration_us=av_rescale_q(stream->duration,stream->time_base,{1,1000000});
        else if(fmt->duration!=AV_NOPTS_VALUE)duration_us=fmt->duration;
        pkt=av_packet_alloc();frame=av_frame_alloc();return pkt && frame;
    }
    bool seek(int64_t us){
        auto *stream=fmt->streams[index];int64_t at=av_rescale_q(us,{1,1000000},stream->time_base);
        if(stream->start_time!=AV_NOPTS_VALUE)at+=stream->start_time;
        int rc=avformat_seek_file(fmt,index,INT64_MIN,at,at,0);
        if(rc<0 && last_block_us>0){
            int64_t earlier=av_rescale_q(std::max<int64_t>(0,us-last_block_us),{1,1000000},stream->time_base);
            if(stream->start_time!=AV_NOPTS_VALUE)earlier+=stream->start_time;
            rc=av_seek_frame(fmt,index,earlier,AVSEEK_FLAG_BACKWARD);
        }
        if(rc<0)return false;
        avcodec_flush_buffers(ctx);av_packet_unref(pkt);av_frame_unref(frame);
        if(swr)swr_free(&swr);av_channel_layout_uninit(&swr_layout);swr_rate=swr_output=0;swr_format=-1;
        input_end=output_end=false;stereo.clear();prepared.clear();stereo_pos=prepared_pos=0;first_media_us=-1;total_frames=delivered_frames=0;return true;
    }
    int finish_read(int count,int64_t pts){
        if(count<0)return -1;stereo.resize(size_t(count)*2);
        if(count){if(first_media_us<0)first_media_us=std::max<int64_t>(0,pts);total_frames+=count;}
        return count || !output_end?1:0;
    }
    int read(int output_rate){
        stereo.clear();stereo_pos=0;if(output_end)return 0;
        for(;;){
            int rc=avcodec_receive_frame(ctx,frame);
            if(rc==0){
                if(frame->sample_rate<=0 || frame->nb_samples<0 || frame->nb_samples>(1<<20) || frame->ch_layout.nb_channels<1 || frame->ch_layout.nb_channels>32){av_frame_unref(frame);return -1;}
                last_block_us=int64_t(frame->nb_samples)*1000000/frame->sample_rate;
                bool same=swr && swr_rate==frame->sample_rate && swr_format==frame->format && swr_output==output_rate && !av_channel_layout_compare(&swr_layout,&frame->ch_layout);
                if(!same){
                    if(swr)swr_free(&swr);av_channel_layout_uninit(&swr_layout);AVChannelLayout layout=AV_CHANNEL_LAYOUT_STEREO;
                    if(swr_alloc_set_opts2(&swr,&layout,AV_SAMPLE_FMT_S16,output_rate,&frame->ch_layout,AVSampleFormat(frame->format),frame->sample_rate,0,nullptr)<0 || !swr || swr_init(swr)<0 || av_channel_layout_copy(&swr_layout,&frame->ch_layout)<0){av_frame_unref(frame);return -1;}
                    swr_rate=frame->sample_rate;swr_format=frame->format;swr_output=output_rate;
                }
                int count=swr_get_out_samples(swr,frame->nb_samples);if(count<0 || count>(1<<20)){av_frame_unref(frame);return -1;}stereo.resize(size_t(count)*2);
                int64_t pts=frame->best_effort_timestamp;if(pts==AV_NOPTS_VALUE)pts=frame->pts;
                auto *stream=fmt->streams[index];
                if(pts!=AV_NOPTS_VALUE){if(stream->start_time!=AV_NOPTS_VALUE)pts-=stream->start_time;pts=av_rescale_q(pts,stream->time_base,{1,1000000})-av_rescale(swr_get_delay(swr,frame->sample_rate),1000000,frame->sample_rate);}
                else pts=first_media_us<0?0:first_media_us+total_frames*1000000/output_rate;
                uint8_t *dest[]={reinterpret_cast<uint8_t*>(stereo.data())};
                int got=swr_convert(swr,dest,count,const_cast<const uint8_t**>(frame->extended_data),frame->nb_samples);av_frame_unref(frame);return finish_read(got,pts);
            }
            if(rc==AVERROR_EOF){
                int count=swr?swr_get_out_samples(swr,0):0;if(count<0 || count>(1<<20))return -1;
                stereo.resize(size_t(count)*2);uint8_t *dest[]={reinterpret_cast<uint8_t*>(stereo.data())};
                int got=swr && count?swr_convert(swr,dest,count,nullptr,0):0;if(got<=0)output_end=true;
                return finish_read(got,first_media_us<0?0:first_media_us+total_frames*1000000/output_rate);
            }
            if((rc!=AVERROR(EAGAIN) && rc!=AVERROR_INVALIDDATA) || input_end)return -1;
            do{rc=av_read_frame(fmt,pkt);if(rc<0)break;if(pkt->stream_index==index)break;av_packet_unref(pkt);}while(true);
            if(rc<0){if(rc!=AVERROR_EOF)return -1;rc=avcodec_send_packet(ctx,nullptr);input_end=rc>=0;}
            else{rc=avcodec_send_packet(ctx,pkt);av_packet_unref(pkt);}
            if(rc==AVERROR_INVALIDDATA)continue; // Discard this bad packet; the demuxer advances.
            if(rc<0)return -1;
        }
    }
    int pull(int16_t *dst,int frames,int output_rate,const std::atomic<bool> &cancel){
        int got=0;
        while(got<frames && !cancel.load()){
            if(prepared_pos<prepared.size()){size_t n=std::min(size_t((frames-got)*2),prepared.size()-prepared_pos);memcpy(dst+got*2,prepared.data()+prepared_pos,n*sizeof(int16_t));prepared_pos+=n;got+=int(n/2);continue;}
            if(stereo_pos<stereo.size()){size_t n=std::min(size_t((frames-got)*2),stereo.size()-stereo_pos);memcpy(dst+got*2,stereo.data()+stereo_pos,n*sizeof(int16_t));stereo_pos+=n;got+=int(n/2);continue;}
            int result=read(output_rate);if(result<0)return -1;if(result==0)break;
        }
        delivered_frames+=got;return got;
    }
};
} // namespace

std::string read_embedded_lyrics(int fd){
    SystemDecode source;source.fd=fd;if(fd<0 || !source.open_format())return {};
    return lyrics_from_context(source.fmt);
}

struct EngineTransition {
    struct Request {
        int fd=-1,rate=0;int64_t track=-1,duration=0;uint64_t generation=0;
        double blend_ms=0;AudioFeatureSettings features;
        std::atomic<bool> cancel{false};bool ok=false;
        std::unique_ptr<SystemDecode> source;
        std::string lyrics;
        ~Request(){if(fd>=0)::close(fd);}
    };
    int rate=0;
    std::mutex mutex;std::condition_variable changed;
    std::shared_ptr<Request> pending,active,ready;
    pthread_t worker{};bool live=false,stopping=false;
    std::atomic<bool> mixing{false},drop_outgoing{false};
    std::unique_ptr<SystemDecode> outgoing;
    int64_t mix_frames=0,mix_at=0;
    void set_rate(int value){rate=value;}
    static void *trampoline(void *user){static_cast<EngineTransition*>(user)->prepare();return nullptr;}
    bool schedule(int fd,int64_t track,int64_t duration,uint64_t generation,double blend_ms,AudioFeatureSettings features){
        std::lock_guard<std::mutex> lock(mutex);
        if(!live){if(pthread_create(&worker,nullptr,trampoline,this)!=0)return false;live=true;}
        if(active)active->cancel=true;if(pending)pending->cancel=true;ready.reset();
        auto next=std::make_shared<Request>();next->fd=fd;next->track=track;next->duration=duration;next->generation=generation;
        next->rate=rate;next->blend_ms=blend_ms;next->features=std::move(features);pending=std::move(next);
        drop_outgoing=true;changed.notify_all();return true;
    }
    void cancel_mix(){outgoing.reset();mixing=false;mix_frames=mix_at=0;changed.notify_all();}
    void reset_seek(int output_rate){cancel_mix();set_rate(output_rate);}
    void prepare(){for(;;){std::shared_ptr<Request> job;
        {std::unique_lock<std::mutex> lock(mutex);changed.wait(lock,[&]{return stopping || (pending && !mixing.load());});if(stopping)return;job=std::move(pending);active=job;}
        auto source=std::make_unique<SystemDecode>();source->fd=job->fd;job->fd=-1;
        if(source->open() && !job->cancel){
            job->lyrics=read_embedded_lyrics(dup(source->fd));job->source=std::move(source);job->ok=true;
        }
        {std::lock_guard<std::mutex> lock(mutex);if(!stopping && !job->cancel)ready=job;active.reset();}
    }}
    std::shared_ptr<Request> take(){
        std::lock_guard<std::mutex> lock(mutex);
        auto out=std::move(ready);if(out && out->ok)mixing=true;return out;
    }
    void begin(const std::shared_ptr<Request> &job,std::unique_ptr<SystemDecode> old){
        outgoing=std::move(old);mixing=true;
        mix_frames=std::max<int64_t>(1,int64_t(rate*job->blend_ms/1000));mix_at=0;
        lyra_log(LyraLogInfo,"Lyra","transition begin crossfade duration_ms=%g",double(mix_frames)*1000/rate);
    }
    int render(SystemDecode &source,int16_t *out,int frames,const std::atomic<bool> &cancel){
        return source.pull(out,frames,rate,cancel);
    }
    void mix(int16_t *incoming,int frames,const std::atomic<bool> &cancel){
        if(!outgoing)return;
        int16_t old[256*2]{};int n=outgoing->pull(old,frames,rate,cancel);if(n<0)n=0;
        audio_crossfade(incoming,n>0?old:nullptr,frames,mix_at,mix_frames);
        mix_at+=frames;
        if(mix_at>=mix_frames){outgoing.reset();mixing=false;changed.notify_all();}
    }

};
Engine::Engine() { sem_init(&sem_, 0, 0); }

Engine::~Engine() {
    stop();
    sem_destroy(&sem_);
}

void Engine::set_listener(void (*fn)(void *user, int event, uint64_t generation), void *user) {
    listener_ = fn;
    listener_user_ = user;
}

void *Engine::tramp(void *self) {
    static_cast<Engine *>(self)->decode_main();
    return nullptr;
}

void Engine::error_cb(void *user, lyra::audio::Result error) { static_cast<Engine *>(user)->on_error(error); }

void Engine::start(int fd, int64_t track, int64_t duration, const AudioFeatureSettings &settings,
                   uint64_t generation, const AudioOutputSettings &output) {
    auto transition_settings=transition_control_.settings();
    std::unique_lock<std::mutex> handoff(handoff_mu_);
    auto before=snapshot(0,false);bool pending_transition=transition_pending_.load();
    if(fd>=0 && transition_settings.enabled && transition_ && accepting_transition_ && thread_live_.load() && !stop_.load() &&
       before.playing() && (pending_transition || (before.clock_ready && !before.seeking)) && seek_ms_.load()<0 && before.sample_rate>0 && start_at_.load()<=0 && !start_paused_.load() &&
       output.capacity_ms==output_settings_.capacity_ms && output.bursts==output_settings_.bursts){
        clock_.access([&](PlaybackClockState &c){
            transition_pending_=true;
            c.generation=generation;c.track_id=track;c.duration_us=std::max<int64_t>(0,duration)*1000;
            c.origin_us=c.frozen_us=0;c.ready=c.timestamp_valid=false;c.seeking=true;c.pcm.reset();c.seek_serial=c.landed_seek=0;++c.revision;
        });
        double remaining=std::max<int64_t>(0,before.duration_us-before.position_us)/1000.;
        double blend=!pending_transition && remaining<=transition_settings.automatic_ms?std::min(remaining,transition_settings.automatic_ms):transition_settings.manual_ms;
        memory_->select(track,0,duration);
        if(transition_->schedule(fd,track,duration,generation,blend,settings)){
            start_at_=-1;start_paused_=false;return;
        }
    }
    handoff.unlock();
    release_writer(false);
    transition_pending_=false;
    if(transition_settings.enabled)transition_=std::make_unique<EngineTransition>();
    {
        std::lock_guard<std::mutex> lock(lyrics_mu_);
        lyrics_text_.clear();
        lyrics_pending_.store(false, std::memory_order_release);
    }
    keep_stream_.store(false, std::memory_order_release);
    while (sem_trywait(&sem_) == 0) {
    }
    feature_settings_ = settings;
    output_settings_ = output;
    pcm_origin_us_ = -1;
    pcm_frames_ = 0;pcm_media_frames_=0;
    stop_.store(false, std::memory_order_relaxed);
    paused_.store(false, std::memory_order_relaxed);
    fault_.store(false, std::memory_order_relaxed);
    recover_output_.store(false, std::memory_order_relaxed);
    finished_.store(false, std::memory_order_relaxed);
    started_.store(false, std::memory_order_relaxed);
    exit_.store(kNone, std::memory_order_relaxed);
    seek_ms_.store(-1, std::memory_order_relaxed);
    bool primed_paused = start_paused_.exchange(false, std::memory_order_acq_rel);
    paused_.store(primed_paused, std::memory_order_release);
    int64_t primed = start_at_.exchange(-1, std::memory_order_acq_rel);
    if (primed >= 0)
        seek_ms_.store(primed, std::memory_order_release);
    clock_.access([&](PlaybackClockState &c) {
        uint64_t revision = c.revision + 1;
        c = {};
        c.track_id = track;
        c.generation = generation;
        c.revision = revision;
        c.duration_us = std::max<int64_t>(0, duration) * 1000;
        c.origin_us = c.frozen_us = audio_ms_us(std::max<int64_t>(0, primed));
        c.active = fd >= 0;
        c.paused = primed_paused;
        c.seeking = primed >= 0;
        c.seek_serial = primed >= 0 ? 1 : 0;
        seek_origin_ = kRestoreSeek;
    });
    if (fd < 0) {
        close_stream();
        finished_.store(true, std::memory_order_release);
        if (listener_)
            listener_(listener_user_, kFailed, generation);
        return;
    }
    memory_->select(track, std::max<int64_t>(0, primed), duration);
    drop_until_us_ = -1;
    drop_blind_ = 0;
    rate_.store(0, std::memory_order_relaxed);
    underruns_.store(0, std::memory_order_relaxed);
    fade_left_.store(0, std::memory_order_relaxed);
    fade_total_.store(0, std::memory_order_relaxed);
    underrun_logged_ = false;
    fd_ = fd;
    if (pthread_create(&thread_, nullptr, &Engine::tramp, this) != 0) {
        ::close(fd);
        fd_ = -1;
        finished_.store(true, std::memory_order_release);
        clock_.access([](PlaybackClockState &c) { c.active = false; });
        close_stream();
        if (listener_)
            listener_(listener_user_, kFailed, generation);
        lyra_log(LyraLogInfo, "Lyra", "play fail thread");
        return;
    }
    thread_live_.store(true, std::memory_order_release);
}

PlaybackBookmark Engine::configure_memory(const std::string &path, int interval_ms) {
    return memory_->configure(path, interval_ms);
}
void Engine::remember_selection(int64_t track, int64_t position, int64_t duration, uint64_t generation) {
    memory_->select(track, position, duration);
    clock_.access([&](PlaybackClockState &c) {
        if (c.active)
            return;
        if (generation)
            c.generation = generation;
        c.track_id = track;
        c.duration_us = std::max<int64_t>(0, duration) * 1000;
        c.origin_us = c.frozen_us = std::max<int64_t>(0, position) * 1000;
        c.origin_frame = c.frozen_frame = c.sampled_frame = 0;
        c.ready = c.timestamp_valid = false;
        c.pcm.reset();
    });
}
PlaybackSnapshot Engine::snapshot(int64_t time_ns, bool with_features) const {
    const int64_t observed_ns = now_ns();
    if (time_ns <= 0)
        time_ns = observed_ns;
    return clock_.access([&](const PlaybackClockState &c) {
        PlaybackSnapshot s = c.for_presentation(observed_ns, time_ns);
        if (with_features && s.playing() && !s.seeking && s.clock_ready && s.output_frame < c.submitted &&
            c.feature_epoch == features_.epoch()) {
            // A slot can be published while reading; retry without re-reading the clock.
            for (int i = 0; i < 3 && !s.features_ready; ++i)
                s.features = features_.sample_output(s.output_frame, &s.features_ready);
        }
        return s;
    });
}
void Engine::reset_features(int rate) {
    clock_.access([&](PlaybackClockState &c) {
        features_.reset(rate, feature_settings_);
        c.rate = rate;
        c.feature_epoch = features_.epoch();
        rate_.store(rate, std::memory_order_release);
    });
}
int64_t Engine::raw_position_ms() const { return snapshot(0, false).position_us / 1000; }
void Engine::checkpoint_progress() {
    PlaybackSnapshot s = snapshot(0, false);
    if (s.active) {
        memory_->update(s.position_us / 1000);
        memory_->checkpoint();
    }
}
AudioFeatures Engine::audio_features(int64_t time_ns, bool *ready) const {
    PlaybackSnapshot s = snapshot(time_ns);
    if (ready)
        *ready = s.features_ready;
    return s.features;
}

void Engine::stop() { release_writer(false); }

void Engine::release_writer(bool keep) {
    checkpoint_progress();
    int64_t mark = now_ms();
    bool live =
        thread_live_.load(std::memory_order_acquire) || stream_.load(std::memory_order_acquire) != nullptr;
    keep_stream_.store(keep, std::memory_order_release);
    exit_.store(kUser, std::memory_order_release);
    stop_.store(true, std::memory_order_release);
    lyra::audio::Stream *stream = nullptr;
    if (keep) {
        std::lock_guard<std::recursive_mutex> control(stream_mu_);
        stream = stream_.load(std::memory_order_acquire);
        if (stream != nullptr) {
            settle_stream(stream);
            lyra::audio::State state = lyra::audio::state(stream);
            if (state == lyra::audio::Started)
                lyra::audio::pause(stream);
        }
    }
    sem_post(&sem_);
    if (thread_live_.exchange(false, std::memory_order_acq_rel))
        pthread_join(thread_, nullptr);
    if (keep) {
        stream = stream_.load(std::memory_order_acquire);
        if (stream != nullptr && !discard_pending(stream))
            close_stream();
    } else {
        close_stream();
    }
    clock_.access([&](PlaybackClockState &c) {
        c.freeze(now_ns());
        c.active = false;
        c.seeking = false;
    });
    transition_.reset();transition_pending_=false;
    finished_.store(true, std::memory_order_release);
    if (live)
        lyra_log(LyraLogInfo, "Lyra", "play stop ms=%lld", (long long)(now_ms() - mark));
}

void Engine::reap() {
    if (!finished_.load(std::memory_order_acquire))
        return;
    if (thread_live_.exchange(false, std::memory_order_acq_rel))
        pthread_join(thread_, nullptr);
    close_stream();transition_.reset();transition_pending_=false;
}

void Engine::pause() {
    std::lock_guard<std::recursive_mutex> control(stream_mu_);
    lyra::audio::Stream *stream = stream_.load(std::memory_order_acquire);
    if (stream != nullptr && started_.load(std::memory_order_acquire) && !settle_stream(stream)) {
        on_error(lyra::audio::ErrorTimeout);
        return;
    }
    PlaybackSnapshot frozen = clock_.access([&](PlaybackClockState &c) {
        auto s = c.sample(now_ns());
        c.frozen_frame = s.output_frame;
        c.frozen_us = s.position_us;
        c.paused = true;
        paused_.store(true, std::memory_order_release);
        return s;
    });
    if (frozen.active) {
        memory_->update(frozen.position_us / 1000);
        memory_->checkpoint();
    }
    if (stream != nullptr && started_.load(std::memory_order_acquire)) {
        auto rc = lyra::audio::pause(stream);
        if (rc != lyra::audio::Ok)
            on_error(rc);
    }
    if (frozen.active)
        lyra_log(LyraLogInfo, "Lyra", "play pause");
}

bool Engine::paused() const { return snapshot(0, false).paused; }

void Engine::seek(int64_t ms, SeekOrigin origin) {
    if (ms < 0)
        ms = 0;
    seek_failed_.store(false, std::memory_order_release);
    // The audible clock moves when the decoder lands, not when the request is posted.
    // Publishing the target here lets the lyric follower run ahead of the samples still in the speaker.
    PlaybackSnapshot before = clock_.access([&](PlaybackClockState &c) {
        auto s = c.sample(now_ns());
        c.frozen_frame=s.output_frame;c.frozen_us=s.position_us;
        c.seeking = true;
        ++c.seek_serial;
        seek_origin_ = origin;
        seek_ms_.store(ms, std::memory_order_release);
        return s;
    });
    // Posting a newer target must not wait behind codec seek/flush. If the
    // writer owns the stream, it will discard the superseded segment itself.
    std::unique_lock<std::recursive_mutex> control(stream_mu_,std::try_to_lock);
    if(control.owns_lock())
        if(auto *audio=stream_.load(std::memory_order_acquire))
            if(lyra::audio::state(audio)==lyra::audio::Started)lyra::audio::pause(audio);
    lyra_log(LyraLogInfo, "Lyra",
             "play seek request origin=%d track=%lld gen=%llu from_us=%lld target_ms=%lld", int(origin),
             (long long)before.track_id, (unsigned long long)before.generation, (long long)before.position_us,
             (long long)ms);
    sem_post(&sem_);
}

void Engine::prime_start(int64_t ms, bool paused) {
    if (ms < 0)
        ms = 0;
    start_at_.store(ms, std::memory_order_release);
    start_paused_.store(paused, std::memory_order_release);
}

void Engine::clear_prime() {
    start_at_.store(-1, std::memory_order_release);
    start_paused_.store(false, std::memory_order_release);
}

bool Engine::take_seek_failed() { return seek_failed_.exchange(false, std::memory_order_acq_rel); }

bool Engine::resume() {
    std::lock_guard<std::recursive_mutex> control(stream_mu_);
    lyra::audio::Stream *stream = stream_.load(std::memory_order_acquire);
    if (stream != nullptr && started_.load(std::memory_order_acquire) && !settle_stream(stream)) {
        on_error(lyra::audio::ErrorTimeout);
        return false;
    }
    bool resumed = clock_.access([&](PlaybackClockState &c) {
        if (!c.paused || !c.active)
            return false;
        if (c.ready)
            c.restart(c.frozen_frame, now_ns());
        c.paused = false;
        paused_.store(false, std::memory_order_release);
        return true;
    });
    if (!resumed)
        return false;
    lyra_log(LyraLogInfo, "Lyra", "play resume");
    sem_post(&sem_);
    if (!started_.load(std::memory_order_acquire))
        maybe_start();
    else {
        // EOF still has queued PCM: restarting its stream is essential to drain it.
        if (stream != nullptr) {
            auto rc = lyra::audio::start(stream);
            if (rc != lyra::audio::Ok) {
                on_error(rc);
                return false;
            }
        }
    }
    return true;
}

bool Engine::timer_active() const { return snapshot(0, false).active; }
double Engine::position_exact() const { return snapshot(0, false).position_us / 1000.; }
int64_t Engine::position_ms() const { return snapshot(0, false).position_us / 1000; }

void Engine::log_underruns() {
    int count = underruns_.exchange(0, std::memory_order_relaxed);
    if (count <= 0 || underrun_logged_)
        return;
    underrun_logged_ = true;
    lyra_log(LyraLogInfo, "Lyra", "play underrun %d", count);
}

void Engine::publish_lyrics(std::string text) {
    std::lock_guard<std::mutex> lock(lyrics_mu_);
    lyrics_text_ = std::move(text);
    lyrics_pending_.store(true, std::memory_order_release);
}

bool Engine::take_lyrics(std::string *out, uint64_t generation, int64_t track) {
    if (out == nullptr || !lyrics_pending_.load(std::memory_order_acquire))
        return false;
    std::lock_guard<std::mutex> lock(lyrics_mu_);
    auto s = snapshot(0, false);
    if (generation != s.generation || track != s.track_id)
        return false;
    if (!lyrics_pending_.exchange(false, std::memory_order_acq_rel))
        return false;
    *out = std::move(lyrics_text_);
    lyrics_text_.clear();
    return true;
}

bool Engine::discard_pending(lyra::audio::Stream *stream) {
    auto wait = [&](lyra::audio::State target) {
        for (int i = 0; i < 4; ++i) {
            auto state = lyra::audio::state(stream);
            if (state == target)
                return true;
            if (state == lyra::audio::Disconnected || state == lyra::audio::Closed)
                return false;
            lyra::audio::State next = state;
            lyra::audio::wait_state(stream, state, &next, 50000000LL);
        }
        return lyra::audio::state(stream) == target;
    };
    auto state = lyra::audio::state(stream);
    if (state == lyra::audio::Open || state == lyra::audio::Flushed)
        return true;
    if (state == lyra::audio::Started || state == lyra::audio::Starting || state == lyra::audio::Pausing) {
        if (state != lyra::audio::Pausing && lyra::audio::pause(stream) != lyra::audio::Ok)
            return false;
        if (!wait(lyra::audio::Paused))
            return false;
    }
    if (lyra::audio::flush(stream) != lyra::audio::Ok)
        return false;
    // A successful request is asynchronous; discarded PCM is gone only after FLUSHED.
    return wait(lyra::audio::Flushed);
}

void Engine::close_stream() {
    std::lock_guard<std::recursive_mutex> control(stream_mu_);
    lyra::audio::Stream *stream = stream_.exchange(nullptr, std::memory_order_acq_rel);
    if (stream == nullptr)
        return;
    if (exit_.load(std::memory_order_acquire) == kUser)
        discard_pending(stream);
    lyra::audio::stop(stream);
    lyra::audio::close(stream);
}

bool Engine::open_output(int preferred_rate) {
    std::lock_guard<std::recursive_mutex> control(stream_mu_);
    auto *existing = stream_.load(std::memory_order_acquire);
    if (existing && preferred_rate >= 8000 && lyra::audio::sample_rate(existing) == preferred_rate &&
        stream_settings_.capacity_ms == output_settings_.capacity_ms &&
        stream_settings_.bursts == output_settings_.bursts) {
        reset_features(preferred_rate);
        last_xruns_ = lyra::audio::xruns(existing);
        lyra_log(LyraLogInfo, "Lyra", "play stream reuse rate=%d", preferred_rate);
        return true;
    }
    if (existing)
        close_stream();
    auto *stream = lyra::audio::open(preferred_rate, output_settings_, &Engine::error_cb, this);
    if (!stream) {
        log_fail("stream", 0);
        return false;
    }
    stream_.store(stream, std::memory_order_release);
    stream_settings_ = output_settings_;
    reset_features(lyra::audio::sample_rate(stream));
    last_xruns_ = lyra::audio::xruns(stream);
    return true;
}

void Engine::feed(const int16_t *src, int count, int64_t frame_us, double speed) {
    if (count < 2 || src == nullptr)
        return;
    if (seek_ms_.load(std::memory_order_acquire) >= 0 || stop_.load(std::memory_order_acquire))
        return;
    int frames = count / 2;
    int rate = rate_.load(std::memory_order_acquire);
    if (drop_until_us_ >= 0 && rate > 0) {
        // A frame with no timestamp used to cancel the drop and play the keyframe
        // while the clock already said the seek target.
        if (frame_us < 0) {
            if (++drop_blind_ < 48)
                return;
            drop_until_us_ = -1;
            drop_blind_ = 0;
        } else {
            drop_blind_ = 0;
            int64_t dur = audio_frames_us(frames, rate);
            if (frame_us + dur <= drop_until_us_)
                return;
            if (frame_us < drop_until_us_) {
                int64_t skip64 = ((drop_until_us_ - frame_us) * (int64_t)rate + 999999) / 1000000;
                if (skip64 < 0)
                    skip64 = 0;
                if (skip64 > frames)
                    skip64 = frames;
                src += (int)skip64 * 2;
                frames -= (int)skip64;
                frame_us += audio_frames_us(skip64, rate);
            }
            drop_until_us_ = -1;
        }
    }
    if (frames <= 0)
        return;
    if (pcm_origin_us_ < 0)
        pcm_origin_us_ = frame_us >= 0 ? frame_us : snapshot(0, false).position_us;
    int off = 0;
    while (off < frames && !stop_.load(std::memory_order_acquire)) {
        if (seek_ms_.load(std::memory_order_acquire) >= 0)
            return;
        if (paused_.load(std::memory_order_acquire)) {
            while (sem_wait(&sem_) != 0 && errno == EINTR) {
            }
            continue;
        }
        if (!started_.load(std::memory_order_acquire) && frame_us >= 0) {
            clock_.access([&](PlaybackClockState &c) { c.origin_us = frame_us; });
        }
        maybe_start();
        lyra::audio::Stream *stream = stream_.load(std::memory_order_acquire);
        if (stream == nullptr)
            return;
        const int16_t *ptr = src + off * 2;
        int chunk = frames - off;
        int16_t faded[256 * 2];
        int left = fade_left_.load(std::memory_order_relaxed);
        int faded_frames = 0;
        if (left > 0) {
            int total = fade_total_.load(std::memory_order_relaxed);
            if (total < 1)
                total = left;
            int use = chunk < 256 ? chunk : 256;
            if (use > left)
                use = left;
            int done = total - left;
            for (int i = 0; i < use; ++i) {
                int gain = done + i;
                if (gain < 0)
                    gain = 0;
                if (gain > total)
                    gain = total;
                faded[i * 2] = (int16_t)((int32_t)ptr[i * 2] * gain / total);
                faded[i * 2 + 1] = (int16_t)((int32_t)ptr[i * 2 + 1] * gain / total);
            }
            ptr = faded;
            chunk = use;
            faded_frames = use;
        } else if (chunk > 4096) {
            chunk = 4096;
        }
        int32_t wrote = lyra::audio::write(stream, ptr, chunk, 20000000LL);
        if (wrote == 0) {
            refresh_clock(stream);
            continue;
        }
        if (wrote < 0) {
            if (paused_.load(std::memory_order_acquire) || stop_.load(std::memory_order_acquire)) {
                timespec delay{0, 2000000};
                nanosleep(&delay, nullptr);
                continue;
            }
            if (wrote == lyra::audio::ErrorInvalidState) {
                std::lock_guard<std::recursive_mutex> control(stream_mu_);
                if (paused_.load(std::memory_order_acquire) || stop_.load(std::memory_order_acquire) ||
                    (settle_stream(stream) && lyra::audio::state(stream) == lyra::audio::Started))
                    continue;
            }
            on_error(wrote);
            return;
        }
        if (faded_frames > 0) {
            int remain = left - wrote;
            if (remain < 0)
                remain = 0;
            fade_left_.store(remain, std::memory_order_relaxed);
        }
        // Keep both PCM media PTS and the output-frame mapping. A flush may advance
        // output counters; sampling must use the current segment's presentation clock.
        int64_t output_end = lyra::audio::written(stream);
        int64_t media_us = pcm_origin_us_ + int64_t(pcm_media_frames_*1000000/rate);
        features_.feed(ptr, wrote, pcm_origin_us_ * rate / 1000000 + int64_t(pcm_media_frames_), output_end - wrote);
        pcm_frames_ += wrote;pcm_media_frames_+=wrote*speed;
        off += wrote;

        int64_t presented = 0, when = 0;
        bool timestamp =
            lyra::audio::timestamp(stream, &presented, &when) == lyra::audio::Ok && presented >= 0;
        int64_t queued = lyra::audio::written(stream);
        {
            clock_.access([&](PlaybackClockState &c) {
                if(transition_pending_.load())return;
                c.pcm.append(output_end - wrote, wrote, media_us, rate,speed);
                c.publish(presented, when, queued, timestamp);
            });
        }
        int xruns = lyra::audio::xruns(stream);
        if (xruns > last_xruns_)
            underruns_.fetch_add(xruns - last_xruns_, std::memory_order_relaxed);
        last_xruns_ = xruns;
        if (!paused_.load(std::memory_order_acquire) && !transition_pending_.load())
            memory_->update(raw_position_ms());
    }
}

void Engine::refresh_clock(lyra::audio::Stream *stream) {
    if(transition_pending_.load())return;
    int64_t frame = 0, stamp = 0;
    bool valid = lyra::audio::timestamp(stream, &frame, &stamp) == lyra::audio::Ok;
    int64_t queued = lyra::audio::written(stream);
    clock_.access([&](PlaybackClockState &c) { c.publish(frame, stamp, queued, valid); });
}

void Engine::wait_for_drain() {
    lyra::audio::Stream *stream = stream_.load(std::memory_order_acquire);
    if (stream == nullptr)
        return;
    int64_t written = lyra::audio::written(stream);
    // A pause in the queued tail is still a pause, not an end-of-track timeout.
    int attempts = 0;
    while (attempts < 250 && !stop_.load(std::memory_order_acquire)) {
        if(transition_pending_.load())return;
        if (seek_ms_.load(std::memory_order_acquire) >= 0)
            return;
        if (!paused_.load(std::memory_order_acquire)) {
            ++attempts;
            int64_t presented = 0, when = 0;
            if (lyra::audio::timestamp(stream, &presented, &when) == lyra::audio::Ok) {
                clock_.access([&](PlaybackClockState &c) { c.publish(presented, when, written, true); });
                memory_->update(raw_position_ms());
                if (clock_.access([&](const PlaybackClockState &c){return c.sample(now_ns()).output_frame;}) >= written)
                    return;
            }
        }
        timespec delay{0, 20000000};
        nanosleep(&delay, nullptr);
    }
}

void Engine::maybe_start() {
    std::lock_guard<std::recursive_mutex> control(stream_mu_);
    if (paused_.load(std::memory_order_acquire) || stop_.load(std::memory_order_acquire) ||
        seek_ms_.load(std::memory_order_acquire) >= 0)
        return;
    lyra::audio::Stream *stream = stream_.load(std::memory_order_acquire);
    if (stream == nullptr)
        return;
    int rate = rate_.load(std::memory_order_acquire);
    if (rate < 1)
        return;
    bool idle = false;
    if (!started_.compare_exchange_strong(idle, true, std::memory_order_acq_rel))
        return;
    int ramp = rate / 125;
    if (ramp < 32)
        ramp = 32;
    fade_total_.store(ramp, std::memory_order_relaxed);
    fade_left_.store(ramp, std::memory_order_release);
    timespec start_time{};
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    int64_t base = std::max<int64_t>(0, lyra::audio::written(stream));
    clock_.access([&](PlaybackClockState &c) {
        c.origin_frame = base;
        c.restart(base, start_time.tv_sec * 1000000000LL + start_time.tv_nsec);
    });
    lyra::audio::Result rc = lyra::audio::start(stream);
    if (rc != lyra::audio::Ok) {
        started_.store(false, std::memory_order_release);
        on_error(rc);
        return;
    }
    lyra_log(LyraLogInfo, "Lyra", "play start rate=%d ms=%lld", rate,
             (long long)(now_ms() - decode_mark_.load(std::memory_order_relaxed)));
}

void Engine::on_error(lyra::audio::Result error) {
    lyra_log(LyraLogInfo, "Lyra", "play stream error %d", (int)error);
    if (error == lyra::audio::ErrorDisconnected && !stop_.load(std::memory_order_acquire)) {
        bool recover = clock_.access([&](PlaybackClockState &c) {
            if (!c.active)
                return false;
            if (recover_output_.exchange(true, std::memory_order_acq_rel))
                return true;
            auto audible = c.sample(now_ns());
            c.origin_us = c.frozen_us = audible.position_us;
            c.origin_frame = c.frozen_frame = audible.output_frame;
            c.ready = false;
            c.timestamp_valid = false;
            c.pcm.reset();
            c.seeking = true;
            if (seek_ms_.load(std::memory_order_acquire) < 0) {
                ++c.seek_serial;
                seek_origin_ = kRecoverySeek;
                seek_ms_.store(audible.position_us / 1000, std::memory_order_release);
            }
            return true;
        });
        if (recover) {
            sem_post(&sem_);
            return;
        }
    }
    fault_.store(true, std::memory_order_release);
    stop_.store(true, std::memory_order_release);
    clock_.access([&](PlaybackClockState &c) {
        c.freeze(now_ns());
        c.active = false;
        c.seeking = false;
    });
    sem_post(&sem_);
}

void Engine::decode_main() {
    decode_mark_.store(now_ms());setpriority(PRIO_PROCESS,0,-16);
    bool ok=false;
    {
        auto source=std::make_unique<SystemDecode>();source->fd=fd_;fd_=-1;
        auto transition_settings=transition_control_.settings();
        if(source->open() && open_output(source->sample_rate)){
            ok=true;
            if(transition_)transition_->set_rate(rate_.load());
            if(source->duration_us>0)clock_.access([&](PlaybackClockState &c){c.duration_us=source->duration_us;});
            publish_lyrics(read_embedded_lyrics(dup(source->fd)));
            auto apply_seek = [&]() {
                auto &d=*source;
                uint64_t serial = 0;
                int64_t ms = clock_.access([&](PlaybackClockState &c) {
                    serial = c.seek_serial;
                    return seek_ms_.exchange(-1);
                });
                if (ms < 0)
                    return;
                std::lock_guard<std::recursive_mutex> guard(stream_mu_);
                auto *audio = stream_.load();
                // A seek starts a new output epoch. Legacy AAudio can retain a
                // timestamp position that excludes flushed frames; reopening avoids
                // accumulating that mismatch across seeks. Track handoffs do not
                // enter here and continue to share their existing stream.
                const int output_rate=rate_.load();
                if (audio) {
                    close_stream();
                    started_.store(false);
                    if (!open_output(output_rate)) {
                        fault_.store(true);
                        stop_.store(true);
                        return;
                    }
                    audio = stream_.load();
                }
                if(!d.seek(audio_ms_us(ms))) {
                    if(!clock_.access([&](const PlaybackClockState &c){return c.seek_serial==serial;}))return;
                    seek_failed_.store(true);fault_.store(true);stop_.store(true);
                    lyra_log(LyraLogError,"Lyra","system decoder seek failed");
                    return;
                }
                recover_output_.store(false);
                started_.store(false);
                int64_t base = audio ? std::max<int64_t>(0, lyra::audio::written(audio)) : 0;
                bool landed=clock_.access([&](PlaybackClockState &c) {
                    if(c.seek_serial!=serial)return false;
                    features_.reset(c.rate, feature_settings_);
                    c.feature_epoch = features_.epoch();
                    c.origin_us = audio_ms_us(ms);
                    c.origin_frame = base;
                    c.presented = c.submitted = c.frozen_frame = c.restart_frame = c.sampled_frame = base;
                    c.stamp_ns = c.restart_ns = 0;
                    c.ready = c.timestamp_valid = false;
                    c.pcm.reset();
                    ++c.revision;
                    c.landed_seek = serial;
                    c.seeking = false;
                    return true;
                });
                if(!landed)return;
                pcm_origin_us_ = -1;
                pcm_frames_ = 0;pcm_media_frames_=0;
                if(transition_)transition_->reset_seek(rate_.load());
                memory_->update(ms);
                memory_->checkpoint();
                drop_until_us_ = ms * 1000;
                drop_blind_ = 0;
            };
            {std::lock_guard<std::mutex> lock(handoff_mu_);accepting_transition_=true;}
            bool begin_target=false;uint64_t source_generation=clock_.access([](const PlaybackClockState &c){return c.generation;});
            while(!stop_.load()){
                auto *transition=transition_.get();
                if(transition && transition->drop_outgoing.exchange(false))transition->cancel_mix();
                if(transition_pending_.load() && transition){
                    auto ready=transition->take();
                    std::unique_lock<std::mutex> accept(handoff_mu_);
                    if(ready && !ready->cancel && clock_.access([&](const PlaybackClockState &c){return c.generation==ready->generation;})){
                        if(!ready->ok){accepting_transition_=false;ok=false;break;}
                        transition->begin(ready,std::move(source));source=std::move(ready->source);
                        source_generation=ready->generation;feature_settings_=ready->features;
                        {std::lock_guard<std::mutex> lock(lyrics_mu_);lyrics_text_.clear();lyrics_pending_=false;}
                        publish_lyrics(std::move(ready->lyrics));
                        begin_target=true;
                    }
                }
                if(begin_target && clock_.access([&](const PlaybackClockState &c){return c.generation!=source_generation;}))begin_target=false;
                if(begin_target){
                    auto base=std::max<int64_t>(0,lyra::audio::written(stream_.load()));
                    pcm_origin_us_=-1;pcm_frames_=0;pcm_media_frames_=0;
                    reset_features(rate_.load());
                    clock_.access([&](PlaybackClockState &c){
                        if(c.generation!=source_generation)return;
                        c.origin_us=c.frozen_us=std::max<int64_t>(0,source->first_media_us);
                        if(source->duration_us>0)c.duration_us=source->duration_us;
                        c.origin_frame=base;c.presented=c.submitted=c.frozen_frame=c.sampled_frame=base;
                        c.ready=c.timestamp_valid=false;c.pcm.reset();c.restart(base,now_ns());c.seeking=seek_ms_.load()>=0;transition_pending_=false;
                    });
                    begin_target=false;
                }
                if(seek_ms_.load()>=0){
                    if(transition_pending_.load()){timespec delay{0,1000000};nanosleep(&delay,nullptr);continue;}
                    apply_seek();continue;
                }
                int16_t pcm[256*2]{};double speed=1;
                int frames=transition?transition->render(*source,pcm,256,stop_):source->pull(pcm,256,rate_.load(),stop_);
                if(frames<0){ok=false;break;}
                if(frames==0){
                    if(transition_pending_.load()){
                        {timespec delay{0,1000000};nanosleep(&delay,nullptr);}continue;
                    }
                    if(started_.load())wait_for_drain();
                    if(seek_ms_.load()>=0 || transition_pending_.load())continue;
                    {std::lock_guard<std::mutex> lock(handoff_mu_);if(transition_pending_.load())continue;accepting_transition_=false;}
                    break;
                }
                double media=double(source->delivered_frames-frames);
                int64_t pts=std::max<int64_t>(0,source->first_media_us)+int64_t(media*1000000/rate_.load());
                if(transition)transition->mix(pcm,frames,stop_);
                feed(pcm,frames*2,pts,speed);
            }
            if(ok && !stop_.load()){memory_->update(0);memory_->checkpoint();}
        }else lyra_log(LyraLogError,"Lyra","system decoder cannot open media");
    }
    {std::lock_guard<std::mutex> lock(handoff_mu_);accepting_transition_=false;}
    if(exit_.load()!=kUser || !keep_stream_.load())close_stream();
    if(fault_.load() || (stop_.load() && exit_.load()==kUser))ok=false;
    uint64_t gen=clock_.access([&](PlaybackClockState &c){c.freeze(now_ns());c.active=false;c.seeking=false;return c.generation;});
    finished_.store(true);
    if(exit_.load()!=kUser && listener_)listener_(listener_user_,ok?kEnded:kFailed,gen);
}
