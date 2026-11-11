#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
root=$(cd ../.. && pwd)
sdk=${ANDROID_HOME:-$HOME/Android/Sdk}
ndk=$sdk/ndk/28.2.13676358
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang++
flavor=${LYRA_FFMPEG_FLAVOR:-ffmpeg}
case "$flavor" in ffmpeg) profile=lite; suffix=-lite;; ffmpeg-full) profile=full; suffix=;; *) exit 1;; esac
metadata="$root/plugins/$flavor"
prefix=${LYRA_FFMPEG_PREFIX:-$PWD/native/third_party/ffmpeg/out$suffix}
if [[ -z ${LYRA_FFMPEG_PREFIX:-} ]]; then LYRA_FFMPEG_PROFILE=$profile bash native/third_party/build_ffmpeg.sh; fi
[[ -s "$prefix/audio-decoders.txt" ]] || { echo "FFmpeg prefix lacks verified audio registry" >&2; exit 1; }
version=$(python3 -c 'import json,tomllib,sys; p=sys.argv[1]; d=json.load(open(p+"/catalog.json")); m=tomllib.load(open(p+"/manifest.toml","rb")); assert d["version"]==m["version"]; print(d["version"])' "$metadata")
output="$root/dist/$flavor"
mkdir -p "$output/licenses"
"$compiler" "-DLYRA_PLUGIN_VERSION=\"$version\"" "-DLYRA_PLUGIN_ID=\"official.$flavor\"" -std=c++17 -Oz -flto -shared -static-libstdc++ -fPIC -fno-exceptions -fno-rtti -fno-stack-protector -fvisibility=hidden -ffunction-sections -fdata-sections -I native -I native/third_party/signalsmith/linear/include -I "$root/sdk/include" -I "$prefix/include" \
 src/plugin.cpp native/playback/{engine,engine_provider,audio_features,progress_memory,transition_control}.cpp native/platform/android/{audio_output,log}.cpp \
 -Wl,--start-group "$prefix/lib/"{libavformat,libavcodec,libswresample,libavutil}.a -Wl,--end-group \
 -Wl,--icf=all -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384 -laaudio -llog -lz -ldl -lm -o "$output/plugin.so"
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded "$output/plugin.so"
cp "$metadata/"{manifest.toml,icon.png,README.md,CHANGELOG.md} "$output/"
cp "$metadata/catalog.json" "$output/metadata.json"
cp "$prefix/audio-decoders.txt" "$output/audio-decoders.txt"
cp native/third_party/signalsmith/LICENSE.*.txt "$output/licenses/"
cp licenses/FFmpeg-LGPL-2.1.txt "$output/LICENSE.txt"
python3 - "$root" "$flavor" <<'PY'
from pathlib import Path
from zipfile import ZipFile,ZIP_DEFLATED
import sys,json
root=Path(sys.argv[1]);flavor=sys.argv[2]
metadata=json.loads((root/'plugins'/flavor/'catalog.json').read_text())
out=root/'dist'/metadata['package']
with ZipFile(out,'w',ZIP_DEFLATED,compresslevel=9) as z:
 for n in ['manifest.toml','plugin.so','LICENSE.txt','metadata.json','icon.png','README.md','CHANGELOG.md','licenses/LICENSE.stretch.txt','licenses/LICENSE.linear.txt','audio-decoders.txt']:z.write(root/'dist'/flavor/n,n)
print(out)
PY
