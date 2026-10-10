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
PREFIX="$WORK/out"
TARBALL="$WORK/ffmpeg-7.1.1.tar.xz"
STAMP="$PREFIX/stamp"
KEY="ffmpeg-7.1.1 fused hidden noproto nodouble nopix aac alac ape flac mp3 opus vorbis wavpack pcm swresample"

if [[ ! -x "$CC" ]]; then
  echo "ndk clang missing: $CC" >&2
  exit 1
fi

if [[ -f "$STAMP" && "$(cat "$STAMP")" == "$KEY" && -f "$PREFIX/lib/libavcodec.a" && -f "$PREFIX/include/libavcodec/avcodec.h" ]]; then
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

python3 - "$SRC" <<'PY'
import pathlib, sys
src = pathlib.Path(sys.argv[1])

def must_replace(path, old, new, label):
    file = src / path
    text = file.read_text()
    if old not in text:
        raise SystemExit(f"patch miss: {label}")
    file.write_text(text.replace(old, new, 1))

must_replace(
    "libavutil/tx.c",
    "    ff_tx_codelet_list_float_c,\n    ff_tx_codelet_list_double_c,\n    ff_tx_codelet_list_int32_c,\n",
    "    ff_tx_codelet_list_float_c,\n    ff_tx_codelet_list_int32_c,\n",
    "tx double codelet",
)

pix = src / "libavutil/pixdesc.c"
text = pix.read_text()
start = text.find("static const AVPixFmtDescriptor av_pix_fmt_descriptors[AV_PIX_FMT_NB] = {")
if start < 0:
    raise SystemExit("patch miss: pixdesc table")
brace = text.find("{", start)
depth = 0
end = None
for i in range(brace, len(text)):
    if text[i] == "{":
        depth += 1
    elif text[i] == "}":
        depth -= 1
        if depth == 0:
            end = i + 1
            break
if end is None or not text.startswith(";", end):
    raise SystemExit("patch miss: pixdesc table end")
text = text[:start] + text[end + 1:]
pix.write_text(text)

must_replace(
    "libavutil/pixdesc.c",
    """static enum AVPixelFormat get_pix_fmt_internal(const char *name)
{
    enum AVPixelFormat pix_fmt;

    for (pix_fmt = 0; pix_fmt < AV_PIX_FMT_NB; pix_fmt++)
        if (av_pix_fmt_descriptors[pix_fmt].name &&
            (!strcmp(av_pix_fmt_descriptors[pix_fmt].name, name) ||
             av_match_name(name, av_pix_fmt_descriptors[pix_fmt].alias)))
            return pix_fmt;

    return AV_PIX_FMT_NONE;
}""",
    """static enum AVPixelFormat get_pix_fmt_internal(const char *name)
{
    (void)name;
    return AV_PIX_FMT_NONE;
}""",
    "pixfmt name lookup",
)
must_replace(
    "libavutil/pixdesc.c",
    """const char *av_get_pix_fmt_name(enum AVPixelFormat pix_fmt)
{
    return (unsigned)pix_fmt < AV_PIX_FMT_NB ?
        av_pix_fmt_descriptors[pix_fmt].name : NULL;
}""",
    """const char *av_get_pix_fmt_name(enum AVPixelFormat pix_fmt)
{
    (void)pix_fmt;
    return NULL;
}""",
    "pixfmt name",
)
must_replace(
    "libavutil/pixdesc.c",
    """        const AVPixFmtDescriptor *pixdesc = &av_pix_fmt_descriptors[pix_fmt];
        snprintf(buf, buf_size, "%-11s %7d %10d", pixdesc->name,
                 pixdesc->nb_components, av_get_bits_per_pixel(pixdesc));""",
    """        if (buf_size > 0)
            buf[0] = 0;""",
    "pixfmt string",
)
must_replace(
    "libavutil/pixdesc.c",
    """const AVPixFmtDescriptor *av_pix_fmt_desc_get(enum AVPixelFormat pix_fmt)
{
    if (pix_fmt < 0 || pix_fmt >= AV_PIX_FMT_NB)
        return NULL;
    return &av_pix_fmt_descriptors[pix_fmt];
}

const AVPixFmtDescriptor *av_pix_fmt_desc_next(const AVPixFmtDescriptor *prev)
{
    if (!prev)
        return &av_pix_fmt_descriptors[0];
    while (prev - av_pix_fmt_descriptors < FF_ARRAY_ELEMS(av_pix_fmt_descriptors) - 1) {
        prev++;
        if (prev->name)
            return prev;
    }
    return NULL;
}

enum AVPixelFormat av_pix_fmt_desc_get_id(const AVPixFmtDescriptor *desc)
{
    if (desc < av_pix_fmt_descriptors ||
        desc >= av_pix_fmt_descriptors + FF_ARRAY_ELEMS(av_pix_fmt_descriptors))
        return AV_PIX_FMT_NONE;

    return desc - av_pix_fmt_descriptors;
}""",
    """const AVPixFmtDescriptor *av_pix_fmt_desc_get(enum AVPixelFormat pix_fmt)
{
    (void)pix_fmt;
    return NULL;
}

const AVPixFmtDescriptor *av_pix_fmt_desc_next(const AVPixFmtDescriptor *prev)
{
    (void)prev;
    return NULL;
}

enum AVPixelFormat av_pix_fmt_desc_get_id(const AVPixFmtDescriptor *desc)
{
    (void)desc;
    return AV_PIX_FMT_NONE;
}""",
    "pixdesc getters",
)
must_replace(
    "libavcodec/decode.c",
    """    desc = av_pix_fmt_desc_get(fmt[n - 1]);
    if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
        return fmt[n - 1];""",
    """    desc = av_pix_fmt_desc_get(fmt[n - 1]);
    if (!desc || !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
        return fmt[n - 1];""",
    "default get_format",
)
must_replace(
    "libavcodec/decode.c",
    """    desc = av_pix_fmt_desc_get(fmt[n - 1]);
    if (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) {""",
    """    desc = av_pix_fmt_desc_get(fmt[n - 1]);
    if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {""",
    "ff_get_format",
)
must_replace(
    "libavcodec/avcodec.c",
    """            if (enc->bits_per_raw_sample && enc->pix_fmt != AV_PIX_FMT_NONE &&
                enc->bits_per_raw_sample < av_pix_fmt_desc_get(enc->pix_fmt)->comp[0].depth)
                av_bprintf(&bprint, "%d bpc, ", enc->bits_per_raw_sample);""",
    """            if (enc->bits_per_raw_sample && enc->pix_fmt != AV_PIX_FMT_NONE) {
                const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(enc->pix_fmt);
                if (desc && enc->bits_per_raw_sample < desc->comp[0].depth)
                    av_bprintf(&bprint, "%d bpc, ", enc->bits_per_raw_sample);
            }""",
    "codec string",
)
must_replace(
    "libavcodec/utils.c",
    """    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(s->pix_fmt);
    int chroma_shift = desc->log2_chroma_w;""",
    """    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(s->pix_fmt);
    int chroma_shift = desc ? desc->log2_chroma_w : 0;""",
    "align dimensions",
)
must_replace(
    "libavformat/demux.c",
    """            if (avctx->codec_id == AV_CODEC_ID_RAWVIDEO && !avctx->codec_tag && !avctx->bits_per_coded_sample) {
                uint32_t tag= avcodec_pix_fmt_to_codec_tag(avctx->pix_fmt);
                if (avpriv_pix_fmt_find(PIX_FMT_LIST_RAW, tag) == avctx->pix_fmt)
                    avctx->codec_tag= tag;
            }

""",
    "",
    "rawvideo tag",
)
print("ffmpeg audio patches applied")
PY

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
  --enable-demuxer=mp3,flac,wav,aiff,aac,mov,ogg,matroska,ape,wv \
  --enable-decoder=mp3,flac,aac,aac_latm,alac,vorbis,opus,ape,wavpack,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_f32le,pcm_f32be,pcm_u8 \
  --enable-parser=mpegaudio,aac,aac_latm,flac,vorbis,opus,ape
then
  echo "---- config.log tail ----" >&2
  tail -n 80 ffbuild/config.log >&2 || true
  exit 1
fi

make -j"$(nproc)"
make install
echo "$KEY" > "$STAMP"
ls -lh "$PREFIX/lib"/*.a
echo "ffmpeg ready"
