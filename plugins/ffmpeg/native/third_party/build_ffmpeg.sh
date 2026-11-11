#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NDK="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/28.2.13676358}"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
API=26
CC="$TOOLCHAIN/bin/aarch64-linux-android${API}-clang"
CXX="$TOOLCHAIN/bin/aarch64-linux-android${API}-clang++"
AR="$TOOLCHAIN/bin/llvm-ar"
RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
STRIP="$TOOLCHAIN/bin/llvm-strip"
NM="$TOOLCHAIN/bin/llvm-nm"
SYSROOT="$TOOLCHAIN/sysroot"
WORK="$ROOT/native/third_party/ffmpeg"
SRC="$WORK/src"
BUILD="$WORK/build"
PROFILE=${LYRA_FFMPEG_PROFILE:-full}
[[ $PROFILE == full || $PROFILE == lite ]] || exit 1
PREFIX="$WORK/out"
if [[ $PROFILE == lite ]]; then PREFIX="$WORK/out-lite"; BUILD="$WORK/build-lite"; fi
TARBALL="$WORK/ffmpeg-7.1.1.tar.xz"
STAMP="$PREFIX/stamp"
KEY="ffmpeg-7.1.1 $PROFILE audio-v4 unmodified-dsp"

if [[ ! -x "$CC" ]]; then
  echo "ndk clang missing: $CC" >&2
  exit 1
fi

if [[ -f "$STAMP" && "$(cat "$STAMP")" == "$KEY" && -f "$PREFIX/audio-decoders.txt" && -f "$PREFIX/lib/libavcodec.a" && -f "$PREFIX/include/libavcodec/avcodec.h" ]]; then
  echo "ffmpeg ready (cached)"
  exit 0
fi

mkdir -p "$WORK"
if [[ ! -f "$TARBALL" ]]; then
  if ! curl -L --fail --retry 3 -o "$TARBALL" "https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz"; then
    curl -L --fail --retry 3 -o "$WORK/ffmpeg-src.tar.gz" "https://github.com/FFmpeg/FFmpeg/archive/refs/tags/n7.1.1.tar.gz"
    TARBALL="$WORK/ffmpeg-src.tar.gz"
  fi
fi

rm -rf "$SRC" "$BUILD"
mkdir -p "$WORK/extract"
rm -rf "$WORK/extract"
mkdir -p "$WORK/extract"
tar -xf "$TARBALL" -C "$WORK/extract"
configure="$(find "$WORK/extract" -maxdepth 2 -type f -name configure | head -n 1)"
if [[ -z "$configure" ]]; then
  echo "ffmpeg configure not found" >&2
  exit 1
fi
mv "$(dirname "$configure")" "$SRC"
rmdir "$WORK/extract" 2>/dev/null || rm -rf "$WORK/extract"
mkdir -p "$BUILD"

# Enumerate upstream's complete built-in audio section (including PCM/DPCM/
# ADPCM), rather than maintaining a lossy hand-written codec whitelist.
AUDIO_DECODERS=$(python3 - "$SRC" <<'PYCODE'
import pathlib,re,sys
s=(pathlib.Path(sys.argv[1])/'libavcodec/allcodecs.c').read_text()
a=s.index('/* audio codecs */');b=s.index('/* subtitles */',a)
names=re.findall(r'extern const FFCodec ff_(\w+)_decoder;',s[a:b])
# Upstream places the Radiance HDR image decoder in this section. Classify
# explicit definitions by media type rather than relying on comments alone.
non_audio=set()
for p in (pathlib.Path(sys.argv[1])/'libavcodec').glob('*.c'):
    for n,typ in re.findall(r'const FFCodec ff_(\w+)_decoder\s*=\s*\{(?:(?!\n\};).)*?\.p\.type\s*=\s*AVMEDIA_TYPE_(\w+)',p.read_text(),re.S):
        if typ!='AUDIO':non_audio.add(n)
names=[n for n in names if n not in non_audio]
if len(names)<100:raise SystemExit('upstream audio codec registry changed')
print(','.join(names))
PYCODE
)

DEMUXERS="--enable-demuxers"
PARSERS="--enable-parsers"
if [[ $PROFILE == lite ]]; then
  AUDIO_DECODERS="mp3,flac,aac,aac_latm,alac,vorbis,opus,ape,wavpack,ac3,eac3,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_f32le,pcm_f32be,pcm_u8"
  DEMUXERS="--enable-demuxer=mp3,flac,wav,aiff,aac,mov,ogg,matroska,ape,wv"
  PARSERS="--enable-parser=mpegaudio,aac,aac_latm,flac,vorbis,opus,ape,ac3"
fi

cd "$BUILD"

if ! "$SRC/configure" \
  --prefix="$PREFIX" \
  --enable-cross-compile \
  --target-os=android \
  --arch=aarch64 \
  --cpu=armv8-a \
  --cc="$CC" \
  --cxx="$CXX" \
  --ar="$AR" \
  --ranlib="$RANLIB" \
  --strip="$STRIP" \
  --nm="$NM" \
  --sysroot="$SYSROOT" \
  --extra-cflags="-Oz -fPIC -ffunction-sections -fdata-sections -fvisibility=hidden -DNDEBUG" \
  --extra-ldflags="-Wl,--gc-sections -Wl,-z,max-page-size=16384" \
  --disable-shared \
  --enable-static \
  --enable-pic \
  --disable-programs \
  --disable-doc \
  --disable-debug \
  --disable-symver \
  --disable-autodetect \
  --disable-everything \
  --disable-avdevice \
  --disable-avfilter \
  --disable-swscale \
  --disable-postproc \
  --disable-network \
  --disable-protocols \
  --disable-encoders \
  --disable-muxers \
  --disable-hwaccels \
  --disable-devices \
  --enable-small \
  --enable-swresample \
  --enable-zlib \
  "$DEMUXERS" \
  --enable-decoder="$AUDIO_DECODERS" \
  "$PARSERS"
then
  echo "---- config.log tail ----" >&2
  tail -n 80 ffbuild/config.log >&2 || true
  exit 1
fi

python3 - "$AUDIO_DECODERS" config_components.h <<'PYCODE'
import pathlib,sys
s=pathlib.Path(sys.argv[2]).read_text()
missing=[n for n in sys.argv[1].split(',') if f'#define CONFIG_{n.upper()}_DECODER 1' not in s]
if missing:raise SystemExit('audio decoders unexpectedly disabled: '+','.join(missing))
print('Verified complete built-in audio registry:',len(sys.argv[1].split(',')))
PYCODE
make -j"${LYRA_FFMPEG_JOBS:-8}"
make install
printf '%s\n' "$AUDIO_DECODERS" > "$PREFIX/audio-decoders.txt"
echo "$KEY" > "$STAMP"
ls -lh "$PREFIX/lib"/*.a
echo "ffmpeg ready"
