// Shared real-decoder fixtures. Caller supplies check/until and WAV writers.
static int edge_fixture(bool silent){
 char path[96];std::snprintf(path,sizeof(path),"/data/local/tmp/lyra-edge-%d.wav",int(getpid()));FILE *f=std::fopen(path,"wb");check(f,"edge fixture");
 int frames=48000*(silent?3:8),bytes=frames*4;
 std::fwrite("RIFF",1,4,f);u32(f,bytes+36);std::fwrite("WAVEfmt ",1,8,f);u32(f,16);u16(f,1);u16(f,2);u32(f,48000);u32(f,192000);u16(f,4);u16(f,16);std::fwrite("data",1,4,f);u32(f,bytes);
 for(int i=0;i<frames;++i){int16_t x=!silent && i>=48000 && i<48000*5?6000:0;u16(f,uint16_t(x));u16(f,uint16_t(-x));}
 std::fclose(f);int fd=::open(path,O_RDONLY);::unlink(path);return fd;
}
static void test_transition_edges(Engine &engine,AudioTransitionSettings settings,const AudioFeatureSettings &features,const Toml &toml){
 std::string error;
 check(toml.require_bool("audio_transition.trim_silence",&settings.edges.enabled,&error),"edge enabled fixture");
 check(toml.require_number("audio_transition.silence_rms_db",&settings.edges.rms_db,&error),"edge RMS fixture");
 check(toml.require_number("audio_transition.silence_peak_db",&settings.edges.peak_db,&error),"edge peak fixture");
 check(toml.require_number("audio_transition.silence_window_ms",&settings.edges.window_ms,&error),"edge window fixture");
 check(toml.require_number("audio_transition.silence_minimum_ms",&settings.edges.minimum_ms,&error),"edge minimum fixture");
 check(toml.require_number("audio_transition.silence_guard_ms",&settings.edges.guard_ms,&error),"edge guard fixture");
 check(toml.require_number("audio_transition.silence_scan_ms",&settings.edges.scan_ms,&error),"edge scan fixture");
 engine.stop();check(configure_transition(engine.transition_interface(),settings)==0,"edge configure");
 int fd=edge_fixture(false);check(fd>=0,"edge fd");engine.start(dup(fd),700,8000,features,700);
 check(until([&]{auto s=engine.snapshot();return s.clock_ready && !s.seeking;}),"edge cold start ready");
 auto initial=engine.snapshot();check(initial.position_us>=850000 && initial.position_us<1500000,"cold start skips silence but retains guard");
 auto end=transition_audible_end(engine.transition_interface(),700,8000000);check(end>=5100000 && end<=5150000,"tail analysis finds effective end");
 engine.start(dup(fd),701,8000,features,701);
 check(until([&]{auto s=engine.snapshot();return s.generation==701 && s.clock_ready && !s.seeking;}),"trimmed incoming transition ready");
 check(engine.snapshot().position_us>=850000,"incoming transition starts at trimmed source position");
 check(transition_audible_end(engine.transition_interface(),700,8000000)==8000000,"old generation cannot borrow new edge metadata");
 engine.seek(0,Engine::kProgressSeek);
 check(until([&]{auto s=engine.snapshot();return s.clock_ready && !s.seeking && s.landed_seek==s.seek_serial;}),"explicit seek to original zero lands");
 check(engine.snapshot().position_us<500000,"explicit seek can revisit original silence");
 engine.stop();::close(fd);fd=edge_fixture(true);engine.start(fd,702,3000,features,702);
 check(until([&]{return engine.snapshot().clock_ready;}),"all-silent short track remains playable");
 check(engine.snapshot().position_us<500000 && transition_audible_end(engine.transition_interface(),702,3000000)==3000000,"all-silent short track retains timeline");
 engine.stop();std::puts("real decoder edge trim: head guard, tail end, phase-inverted stereo, original seek and silent track passed");
}
