#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
root=$(cd ../.. && pwd)
sdk=${ANDROID_HOME:-$HOME/Android/Sdk}
ndk=$sdk/ndk/28.2.13676358
compiler=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang++
version=$(python3 -c 'import json,tomllib; d=json.load(open("catalog.json")); m=tomllib.load(open("manifest.toml","rb")); assert d["version"]==m["version"]; print(d["version"])')
mkdir -p "$root/dist/netease"
"$compiler" "-DLYRA_PLUGIN_VERSION=\"$version\"" -std=c++17 -Oz -flto -shared -static-libstdc++ -fPIC -fno-exceptions -fno-rtti -fvisibility=hidden -ffunction-sections -fdata-sections -I src -I "$root/sdk/include" \
 src/plugin.cpp src/net_http.cpp src/netease_json.cpp src/crypto.cpp \
 -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384 -llog -lz -o "$root/dist/netease/plugin.so"
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" --strip-unneeded "$root/dist/netease/plugin.so"
cp manifest.toml "$root/dist/netease/manifest.toml"
cp icon.png README.md CHANGELOG.md "$root/dist/netease/"
cp catalog.json "$root/dist/netease/metadata.json"
cp licenses/MIT.txt "$root/dist/netease/LICENSE.txt"
python3 - "$root" <<'PY'
from pathlib import Path
from zipfile import ZipFile,ZIP_DEFLATED
import sys
root=Path(sys.argv[1]);import json
version=json.loads((root/'plugins/netease/catalog.json').read_text())['version']
out=root/f'dist/official.netease-{version}-android-arm64-v8a.zip'
with ZipFile(out,'w',ZIP_DEFLATED,compresslevel=9) as z:
 for n in ['manifest.toml','plugin.so','LICENSE.txt','metadata.json','icon.png','README.md','CHANGELOG.md']:z.write(root/'dist/netease'/n,n)
print(out)
PY
