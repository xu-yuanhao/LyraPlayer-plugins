// Test-only loader; production validates these fields in config/codec.cpp.
static void load_full_settings(const Toml &toml,AudioTransitionSettings &s){std::string error;
 check(toml.require_bool("audio_transition.full_enabled",&s.full.enabled,&error),"full enabled");
 check(toml.require_bool("audio_transition.full_library",&s.full.library,&error),"full library");
 check(toml.require_number("audio_transition.full_hop_ms",&s.full.hop_ms,&error),"full hop_ms");
 check(toml.require_number("audio_transition.full_novelty_ms",&s.full.novelty_ms,&error),"full novelty_ms");
 check(toml.require_number("audio_transition.full_novelty_threshold",&s.full.novelty_threshold,&error),"full novelty_threshold");
 check(toml.require_number("audio_transition.full_phrase_min_ms",&s.full.phrase_min_ms,&error),"full phrase_min_ms");
 check(toml.require_number("audio_transition.full_max_ms",&s.full.max_ms,&error),"full max_ms");
 check(toml.require_number("audio_transition.full_cache_mb",&s.full.cache_mb,&error),"full cache_mb");
 check(toml.require_number("audio_transition.full_queue_limit",&s.full.queue_limit,&error),"full queue_limit");
 check(toml.require_number("audio_transition.full_idle_ms",&s.full.idle_ms,&error),"full idle_ms");
 check(toml.require_number("audio_transition.full_vocal_threshold",&s.full.vocal_threshold,&error),"full vocal_threshold");
 check(toml.require_number("audio_transition.full_vocal_floor_db",&s.full.vocal_floor_db,&error),"full vocal_floor_db");
 check(toml.require_number("audio_transition.full_vocal_smooth_ms",&s.full.vocal_smooth_ms,&error),"full vocal_smooth_ms");
 check(toml.require_number("audio_transition.full_vocal_guard_ms",&s.full.vocal_guard_ms,&error),"full vocal_guard_ms");
 check(toml.require_number("audio_transition.full_vocal_fade_ms",&s.full.vocal_fade_ms,&error),"full vocal_fade_ms");
 check(toml.require_number("audio_transition.full_wait_ms",&s.full.wait_ms,&error),"full wait_ms");
 check(toml.require_number("audio_transition.full_intro_ms",&s.full.intro_ms,&error),"full intro_ms");
 check(toml.require_number("audio_transition.full_stem_window_ms",&s.full.stem_window_ms,&error),"full stem_window_ms");
 check(toml.require_number("audio_transition.full_confidence",&s.full.confidence,&error),"full confidence");
 check(toml.require_number("audio_transition.full_chroma_low_hz",&s.full.chroma_low_hz,&error),"full chroma_low_hz");
 check(toml.require_number("audio_transition.full_chroma_high_hz",&s.full.chroma_high_hz,&error),"full chroma_high_hz");
 check(toml.require_number("audio_transition.full_worker_nice",&s.full.worker_nice,&error),"full worker_nice");
 check(audio_transition_valid(s),"full settings valid");}
