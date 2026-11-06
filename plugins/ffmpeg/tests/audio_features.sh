#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
serial=${1:?usage: device_audio_feature_checks.sh adb-serial}
sdk=${ANDROID_HOME:-/home/xu-yuanhao/Android/Sdk}
compiler=$sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang++
out=$root/../../dist/tests/audio-features
mkdir -p "$out"
flags=(-std=c++17 -Oz -flto -UNDEBUG -fno-exceptions -fno-rtti -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables -I "$root/native")
for variant in reference neon; do
    extra=(-DFEATURE_PREFIX="$variant")
    if [[ $variant == reference ]]; then extra+=(-DLYRA_AUDIO_FEATURES_REFERENCE -DAudioFeatureHistory=ReferenceAudioFeatureHistory); fi
    "$compiler" "${flags[@]}" "${extra[@]}" -c "$root/native/playback/audio_features.cpp" -o "$out/$variant.o"
    "$compiler" "${flags[@]}" "${extra[@]}" -c "$root/tests/audio_features_simd_bridge_test.cpp" -o "$out/$variant-bridge.o"
done
"$compiler" "${flags[@]}" "$root/tests/audio_features_simd_test.cpp" "$out/reference.o" "$out/reference-bridge.o" "$out/neon.o" "$out/neon-bridge.o" -static-libstdc++ -Wl,--icf=all -o "$out/test"
remote=/data/local/tmp/lyra-ffmpeg-audio-features-test
trap 'adb -s "$serial" shell rm -f "$remote" >/dev/null 2>&1 || true' EXIT
adb -s "$serial" push "$out/test" "$remote" >/dev/null
adb -s "$serial" shell chmod 755 "$remote"
adb -s "$serial" shell "$remote" | tee "$out/result.txt"
