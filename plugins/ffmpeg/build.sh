#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
root=$(cd ../.. && pwd)
sdk=${ANDROID_HOME:-$HOME/Android/Sdk}
ndk=$sdk/ndk/28.2.13676358
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang++
prefix=${LYRA_FFMPEG_PREFIX:-$PWD/native/third_party/ffmpeg/out}
if [[ ! -f $prefix/lib/libavcodec.a ]]; then bash native/third_party/build_ffmpeg.sh; fi
mkdir -p "$root/dist/ffmpeg"
"$compiler" -std=c++17 -Oz -flto -shared -static-libstdc++ -fPIC -fno-exceptions -fno-rtti -fvisibility=hidden -ffunction-sections -fdata-sections -I native -I "$root/sdk/include" -I "$prefix/include" \
 src/plugin.cpp native/playback/{engine,engine_provider,audio_features,progress_memory}.cpp native/platform/android/{audio_output,log}.cpp \
 -Wl,--start-group "$prefix/lib/"{libavformat,libavcodec,libswresample,libavutil}.a -Wl,--end-group \
 -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384 -laaudio -llog -lz -ldl -lm -o "$root/dist/ffmpeg/plugin.so"
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded "$root/dist/ffmpeg/plugin.so"
cp manifest.toml "$root/dist/ffmpeg/manifest.toml"
cp licenses/FFmpeg-LGPL-2.1.txt "$root/dist/ffmpeg/LICENSE.txt"
python3 - "$root" <<'PY'
from pathlib import Path
from zipfile import ZipFile,ZIP_DEFLATED
import sys
root=Path(sys.argv[1]);out=root/'dist/official.ffmpeg-1.0.0-android-arm64-v8a.zip'
with ZipFile(out,'w',ZIP_DEFLATED,compresslevel=9) as z:
 for n in ['manifest.toml','plugin.so','LICENSE.txt']:z.write(root/'dist/ffmpeg'/n,n)
print(out)
PY
