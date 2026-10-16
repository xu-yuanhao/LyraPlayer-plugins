// Test-only TOML loader. Production configuration is validated by config/codec.cpp.
static void load_adaptive_settings(const Toml &toml,AudioTransitionSettings &s){std::string error;
 check(toml.require_bool("audio_transition.adaptive_enabled",&s.adaptive.enabled,&error),"adaptive enabled");
 check(toml.require_number("audio_transition.adaptive_confidence",&s.adaptive.confidence,&error),"adaptive confidence");
 check(toml.require_number("audio_transition.adaptive_split_hz",&s.adaptive.split_hz,&error),"adaptive split_hz");
 check(toml.require_number("audio_transition.adaptive_smooth_ms",&s.adaptive.smooth_ms,&error),"adaptive smooth_ms");
 check(toml.require_number("audio_transition.adaptive_tolerance_ms",&s.adaptive.tolerance_ms,&error),"adaptive tolerance_ms");
 check(toml.require_number("audio_transition.adaptive_phase_wait_ms",&s.adaptive.phase_wait_ms,&error),"adaptive phase_wait_ms");
 check(toml.require_number("audio_transition.adaptive_echo_wet",&s.adaptive.echo_wet,&error),"adaptive echo_wet");
 check(toml.require_number("audio_transition.adaptive_echo_feedback",&s.adaptive.echo_feedback,&error),"adaptive echo_feedback");
 check(toml.require_number("audio_transition.adaptive_echo_beats",&s.adaptive.echo_beats,&error),"adaptive echo_beats");
 check(toml.require_number("audio_transition.adaptive_echo_max_ms",&s.adaptive.echo_max_ms,&error),"adaptive echo_max_ms");
 check(toml.require_number("audio_transition.adaptive_echo_low_hz",&s.adaptive.echo_low_hz,&error),"adaptive echo_low_hz");
 check(toml.require_number("audio_transition.adaptive_filter_min_hz",&s.adaptive.filter_min_hz,&error),"adaptive filter_min_hz");
 check(toml.require_number("audio_transition.adaptive_filter_max_hz",&s.adaptive.filter_max_hz,&error),"adaptive filter_max_hz");
 check(toml.require_number("audio_transition.adaptive_gain_min",&s.adaptive.gain_min,&error),"adaptive gain_min");
 check(toml.require_number("audio_transition.adaptive_gain_max",&s.adaptive.gain_max,&error),"adaptive gain_max");
 check(toml.require_number("audio_transition.adaptive_mid_start",&s.adaptive.mid_start,&error),"adaptive mid_start");
 check(toml.require_number("audio_transition.adaptive_mid_end",&s.adaptive.mid_end,&error),"adaptive mid_end");
 check(toml.require_number("audio_transition.adaptive_loop_ms",&s.adaptive.loop_ms,&error),"adaptive loop_ms");
 check(toml.require_number("audio_transition.adaptive_bridge_max_ms",&s.adaptive.bridge_max_ms,&error),"adaptive bridge_max_ms");
 check(toml.require_number("audio_transition.adaptive_bridge_fade_ms",&s.adaptive.bridge_fade_ms,&error),"adaptive bridge_fade_ms");
 check(toml.require_number("audio_transition.adaptive_loop_cross_ms",&s.adaptive.loop_cross_ms,&error),"adaptive loop_cross_ms");
 check(toml.require_number("audio_transition.adaptive_cache_entries",&s.adaptive.cache_entries,&error),"adaptive cache_entries");
 check(audio_transition_valid(s),"adaptive settings valid");}
