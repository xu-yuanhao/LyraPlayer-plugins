#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
serial=${1:-c2208dff}
root=$(cd ../.. && pwd)
sdk=${ANDROID_HOME:-$HOME/Android/Sdk}
cxx=$sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang++
prefix=${LYRA_FFMPEG_PREFIX:-$PWD/native/third_party/ffmpeg/out}
mkdir -p "$root/dist/tests"
"$cxx" -std=c++17 -O2 -fno-exceptions -fno-rtti -static-libstdc++ -DLYRA_PLUGIN_VERSION='"1.3.0"' \
 -I native -I "$root/sdk/include" -I native/third_party/signalsmith/linear/include -I "$prefix/include" \
 native/config/toml.cpp src/plugin.cpp native/playback/{engine_test,adaptive_runtime_test,engine_provider,transition_control,audio_features,progress_memory}.cpp native/platform/android/log.cpp \
 -Wl,--start-group "$prefix/lib/"{libavformat,libavcodec,libswresample,libavutil}.a -Wl,--end-group -lz -lm -ldl -llog -latomic -o "$root/dist/tests/engine"
remote=/data/local/tmp/lyra-ffmpeg-transition-$$
trap 'adb -s "$serial" shell rm -f "$remote" "$remote.toml" "$remote.flac" >/dev/null' EXIT
ffmpeg -hide_banner -loglevel error -f lavfi -i 'sine=frequency=440:duration=4:sample_rate=44100' -ac 2 -c:a flac -y "$root/dist/tests/transition.flac"
adb -s "$serial" push "$root/dist/tests/engine" "$remote"
adb -s "$serial" push transition-test.toml "$remote.toml"
adb -s "$serial" push "$root/dist/tests/transition.flac" "$remote.flac"
formats=(mp3 m4a alac.m4a ogg opus wv s24.wav f32.wav)
codecs=(libmp3lame aac alac libvorbis libopus wavpack pcm_s24le pcm_f32le)
args=()
for i in "${!formats[@]}"; do
    file="$root/dist/tests/matrix.${formats[$i]}"
    ffmpeg -hide_banner -loglevel error -f lavfi -i 'sine=frequency=440:duration=4:sample_rate=44100' -ac 2 -c:a "${codecs[$i]}" -y "$file"
    adb -s "$serial" push "$file" "$remote.${formats[$i]}"
    args+=("$remote.${formats[$i]}")
    file="$root/dist/tests/matrix.edge.${formats[$i]}"
    ffmpeg -hide_banner -loglevel error -f lavfi -i 'sine=frequency=440:duration=4:sample_rate=44100' -af 'adelay=1000:all=1,apad=pad_dur=3' -ac 2 -c:a "${codecs[$i]}" -y "$file"
    adb -s "$serial" push "$file" "$remote.edge.${formats[$i]}"
    args+=("$remote.edge.${formats[$i]}")
done
trap 'adb -s "$serial" shell rm -f "$remote" "$remote.toml" "$remote.flac" "${args[@]}" >/dev/null' EXIT
adb -s "$serial" shell "$remote" "$remote.toml" "$remote.flac" "${args[@]}"
