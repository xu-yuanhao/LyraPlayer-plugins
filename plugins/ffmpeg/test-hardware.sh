#!/usr/bin/env bash
# Real AAudio, but only zeros are written to the device.
set -euo pipefail
cd "$(dirname "$0")"
serial=${1:-c2208dff}
root=$(cd ../.. && pwd)
sdk=${ANDROID_HOME:-$HOME/Android/Sdk}
cxx=$sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang++
prefix=${LYRA_FFMPEG_PREFIX:-$PWD/native/third_party/ffmpeg/out}
mkdir -p "$root/dist/tests"
"$cxx" -std=c++17 -O2 -fno-exceptions -fno-rtti -static-libstdc++ -I native -I "$root/sdk/include" \
 -I native/third_party/signalsmith/linear/include -I "$prefix/include" native/config/toml.cpp \
 native/playback/{repeated_seek_hardware_test,silent_hardware_output_test,engine,transition_control,audio_features,progress_memory}.cpp native/platform/android/log.cpp \
 -Wl,--start-group "$prefix/lib/"{libavformat,libavcodec,libswresample,libavutil}.a -Wl,--end-group -laaudio -lz -lm -ldl -llog -latomic -o "$root/dist/tests/hardware"
remote=/data/local/tmp/lyra-ffmpeg-hardware-$$
trap 'adb -s "$serial" shell rm -f "$remote" "$remote.toml" "$remote.flac" >/dev/null' EXIT
ffmpeg -hide_banner -loglevel error -f lavfi -i 'sine=frequency=440:duration=4:sample_rate=44100' -ac 2 -c:a flac -y "$root/dist/tests/transition.flac"
adb -s "$serial" push "$root/dist/tests/hardware" "$remote"
adb -s "$serial" push transition-test.toml "$remote.toml"
adb -s "$serial" push "$root/dist/tests/transition.flac" "$remote.flac"
for rate in 48000 44100; do
    adb -s "$serial" shell "$remote" "$remote.toml" "$remote.flac" "$rate"
done
