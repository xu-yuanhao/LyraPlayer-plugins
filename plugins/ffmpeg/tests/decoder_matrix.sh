#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
serial=${1:-c2208dff}
ndk=${ANDROID_HOME:-$HOME/Android/Sdk}/ndk/28.2.13676358
cxx=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang++
prefix=${LYRA_FFMPEG_PREFIX:-$PWD/plugins/ffmpeg/native/third_party/ffmpeg/out}
mkdir -p dist/tests
flags=(-std=c++17 -O2 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -static-libstdc++ -I plugins/ffmpeg/native -I sdk/include -I plugins/ffmpeg/native/third_party/signalsmith/linear/include -I "$prefix/include")
libs=(-Wl,--gc-sections -Wl,--start-group "$prefix/lib/"{libavformat,libavcodec,libswresample,libavutil}.a -Wl,--end-group -lz -lm -llog -laaudio)
"$cxx" "${flags[@]}" plugins/ffmpeg/tests/decode_file.cpp plugins/ffmpeg/native/platform/android/log.cpp "${libs[@]}" -o dist/tests/decode-file
"$cxx" "${flags[@]}" plugins/ffmpeg/tests/decoder_coverage.cpp "${libs[@]}" -o dist/tests/decoder-coverage
remote=/data/local/tmp/lyra-codec-matrix-$$
adb -s "$serial" shell mkdir -p "$remote"
trap 'adb -s "$serial" shell rm -rf "$remote" >/dev/null' EXIT
adb -s "$serial" push dist/tests/decode-file dist/tests/decoder-coverage "$prefix/audio-decoders.txt" "$remote/" >/dev/null
adb -s "$serial" shell "$remote/decoder-coverage $remote/audio-decoders.txt"
formats=(flac mp3 m4a alac.m4a ogg opus wv ac3 eac3 s24.wav f32.wav u8.wav aiff au caf wma mp2 adpcm.wav truehd mka)
codecs=(flac libmp3lame aac alac libvorbis libopus wavpack ac3 eac3 pcm_s24le pcm_f32le pcm_u8 pcm_s16be pcm_mulaw pcm_s32le wmav2 mp2 adpcm_ima_wav truehd flac)
if [[ ${LYRA_FFMPEG_PROFILE:-full} == lite ]]; then
 formats=(flac mp3 m4a alac.m4a ogg opus wv ac3.mp4 eac3.mp4 s24.wav f32.wav u8.wav aiff)
 codecs=(flac libmp3lame aac alac libvorbis libopus wavpack ac3 eac3 pcm_s24le pcm_f32le pcm_u8 pcm_s16be)
fi
for i in "${!formats[@]}";do
    file="dist/tests/matrix.${formats[$i]}"
    extra=(); if [[ ${codecs[$i]} == truehd ]]; then extra=(-f truehd); fi
    ffmpeg -hide_banner -loglevel error -f lavfi -i sine=frequency=440:duration=4:sample_rate=44100 -ac 2 -c:a "${codecs[$i]}" -strict -2 "${extra[@]}" -y "$file"
    adb -s "$serial" push "$file" "$remote/sample" >/dev/null
    adb -s "$serial" shell "$remote/decode-file $remote/sample"
done

# A metadata-only FLAC may advertise a real duration but carries no audio.
python3 - <<'PYCODE'
from pathlib import Path
s=Path('dist/tests/matrix.flac').read_bytes();end=4
while True:
    tag=s[end];size=int.from_bytes(s[end+1:end+4],'big');end+=4+size
    if tag&128:break
Path('dist/tests/metadata-only.flac').write_bytes(s[:end])
PYCODE
adb -s "$serial" push dist/tests/metadata-only.flac "$remote/empty" >/dev/null
adb -s "$serial" shell "$remote/decode-file --expect-empty $remote/empty"
