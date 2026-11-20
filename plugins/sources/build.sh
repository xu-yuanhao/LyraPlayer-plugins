#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
root=$(cd ../.. && pwd)
sdk=${ANDROID_HOME:-$HOME/Android/Sdk}
bin=$sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin
work=$root/dist/sources
deps=$work/dependencies
mkdir -p "$deps"
fetch() {
    local url=$1 file=$2 digest=$3
    if [[ ! -f $file ]]; then curl -fLsS "$url" -o "$file.tmp" && mv "$file.tmp" "$file"; fi
    printf '%s  %s\n' "$digest" "$file" | sha256sum -c
}
fetch https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz "$deps/quickjs.tar.xz" b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a
fetch https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp "$deps/json.hpp" 9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
fetch https://raw.githubusercontent.com/nlohmann/json/v3.11.3/LICENSE.MIT "$deps/json.LICENSE" 86b998c792894ccb911a1cb7994f7a9652894e7a094c0b5e45be2f553f45cf14
tar -xf "$deps/quickjs.tar.xz" -C "$deps"
quick=$deps/quickjs-2026-06-04
npm --prefix js-runtime ci --no-audit --no-fund
js-runtime/node_modules/.bin/esbuild js-runtime/entry.js --bundle --platform=browser --format=iife \
    --global-name=CeruRuntime --minify --legal-comments=inline --outfile="$work/ceru-runtime.js"
python3 - "$work/adapter.inc" "$work/ceru-runtime.js" <<'PY'
from pathlib import Path
import sys
data=Path("src/web-globals.js").read_bytes()+Path(sys.argv[2]).read_bytes()+Path("src/adapter.js").read_bytes()
Path(sys.argv[1]).write_text("static constexpr unsigned char adapter[]={"+",".join(map(str,data))+",0};\n")
PY
objects=()
for name in quickjs libregexp libunicode cutils dtoa; do
    object=$work/$name.o
    "$bin/aarch64-linux-android26-clang" -fPIC -Oz -fvisibility=hidden -ffunction-sections -fdata-sections -D_GNU_SOURCE '-DCONFIG_VERSION="2026-06-04"' -c "$quick/$name.c" -o "$object"
    objects+=("$object")
done
"$bin/aarch64-linux-android26-clang++" -std=c++17 -Oz -fPIC -shared -static-libstdc++ -fvisibility=hidden -ffunction-sections -fdata-sections \
    -I "$quick" -I "$deps" -I "$work" -I "$root/sdk/include" src/{engine,platform,plugin}.cpp "${objects[@]}" \
    -Wl,--gc-sections -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384 -lm -o "$work/plugin.so"
"$bin/llvm-strip" --strip-unneeded "$work/plugin.so"
cp "$quick/LICENSE" "$work/QUICKJS-LICENSE.txt"
cp "$deps/json.LICENSE" "$work/JSON-LICENSE.txt"
python3 - "$root" <<'PY'
import json,sys,tomllib,zipfile
from pathlib import Path
root=Path(sys.argv[1]); src=Path.cwd()
description=json.loads((src/'catalog.json').read_text())
with (src/'manifest.toml').open('rb') as fh: manifest=tomllib.load(fh)
assert description['version']==manifest['version']
archive=root/'dist'/description['package']
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
    for name in ('manifest.toml','README.md','CHANGELOG.md','icon.png'):
        z.write(src/name,name)
    z.write(src/'catalog.json','metadata.json')
    modules=src/'js-runtime/node_modules'
    for package in ('@shiqianjiang/ceru-plugin-sdk','buffer','crypto-js','fflate','json5',
                    'whatwg-url','webidl-conversions','tr46','punycode','base64-js','ieee754'):
        location=modules/package
        for license in sorted(location.glob('LICENSE*')):
            z.write(license,'licenses/'+package.replace('/','-')+'-'+license.name)

    for name in ('plugin.so','QUICKJS-LICENSE.txt','JSON-LICENSE.txt'):
        z.write(root/'dist/sources'/name,name)
print(archive)
PY
