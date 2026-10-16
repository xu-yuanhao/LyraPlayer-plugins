#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
root=$(cd ../.. && pwd)
sdk=${ANDROID_HOME:-$HOME/Android/Sdk}
compiler=$sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang++
prefix=${LYRA_NCNN_PREFIX:-$PWD/native/third_party/ncnn/out}
if [[ ! -f $prefix/lib/libncnn.a || ! -f models/vocals.bin ]]; then echo 'Run prepare.py to obtain the pinned ncnn dependency and trained model.' >&2;exit 1;fi
version=$(python3 -c 'import json,tomllib; d=json.load(open("catalog.json")); m=tomllib.load(open("manifest.toml","rb")); assert d["version"]==m["version"]; print(d["version"])')
mkdir -p "$root/dist/transition-stems/models" "$root/dist/transition-stems/licenses"
"$compiler" "-DLYRA_PLUGIN_VERSION=\"$version\"" -std=c++17 -Oz -flto -shared -static-libstdc++ -fPIC -fno-exceptions -fno-rtti -fvisibility=hidden -ffunction-sections -fdata-sections \
 -I "$root/sdk/include" -I "$prefix/include/ncnn" -I ../ffmpeg/native \
 src/plugin.cpp -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384 -ldl -llog -landroid -o "$root/dist/transition-stems/plugin.so"
"$sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded "$root/dist/transition-stems/plugin.so"
"$compiler" -std=c++17 -Oz -flto -static-libstdc++ -fPIE -pie -fno-exceptions -fno-rtti -fvisibility=hidden -ffunction-sections -fdata-sections \
 -I "$root/sdk/include" -I "$prefix/include/ncnn" -I ../ffmpeg/native src/worker.cpp "$prefix/lib/libncnn.a" \
 -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384 -ldl -llog -landroid -o "$root/dist/transition-stems/worker"
"$sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded "$root/dist/transition-stems/worker"
cp models/vocals.param models/vocals.bin "$root/dist/transition-stems/models/"
cp manifest.toml "$root/dist/transition-stems/"
if [[ -f README.md ]];then cp README.md "$root/dist/transition-stems/";fi
cp licenses/* "$root/dist/transition-stems/licenses/"

cp catalog.json "$root/dist/transition-stems/metadata.json"
cp CHANGELOG.md "$root/dist/transition-stems/"
python3 - "$root" "$version" <<'PYZIP'
from pathlib import Path
from zipfile import ZipFile,ZIP_DEFLATED
import sys
root=Path(sys.argv[1]);base=root/'dist/transition-stems'
out=root/f'dist/official.transition-stems-{sys.argv[2]}-android-arm64-v8a.zip'
with ZipFile(out,'w',ZIP_DEFLATED,compresslevel=9) as z:
 for path in sorted(base.rglob('*')):
  if path.is_file():z.write(path,path.relative_to(base).as_posix())
print(out)
PYZIP
