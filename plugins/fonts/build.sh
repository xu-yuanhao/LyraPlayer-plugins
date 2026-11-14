#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
root=$(cd ../.. && pwd)
sdk=${ANDROID_HOME:-$HOME/Android/Sdk}
ndk=$sdk/ndk/28.2.13676358
tools=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin
mkdir -p "$root/dist/fonts"
python3 generate_strings.py
"$tools/aarch64-linux-android26-clang++" -std=c++17 -Oz -flto -shared -static-libstdc++ -fPIC -fno-exceptions -fno-rtti -fno-stack-protector -fvisibility=hidden -ffunction-sections -fdata-sections -I src -I "$root/sdk/include" src/plugin.cpp src/platform.cpp src/font_catalog.cpp src/font_metadata.cpp src/font_import.cpp -Wl,--gc-sections -Wl,--icf=all -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384 -o "$root/dist/fonts/plugin.so"
"$tools/llvm-strip" --strip-unneeded "$root/dist/fonts/plugin.so"
cp manifest.toml README.md CHANGELOG.md icon.png "$root/dist/fonts/"
cp catalog.json "$root/dist/fonts/metadata.json"
cp ../netease/licenses/MIT.txt "$root/dist/fonts/LICENSE.txt"
python3 - "$root" <<'PY'
from pathlib import Path
from zipfile import ZipFile,ZIP_DEFLATED
import sys
root=Path(sys.argv[1]);out=root/'dist/official.fonts-1.0.1-android-arm64-v8a.zip'
with ZipFile(out,'w',ZIP_DEFLATED,compresslevel=9) as z:
 for n in ['manifest.toml','plugin.so','LICENSE.txt','metadata.json','README.md','CHANGELOG.md','icon.png']:z.write(root/'dist/fonts'/n,n)
print(out)
PY
