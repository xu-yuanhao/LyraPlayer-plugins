#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
root=$(cd ../.. && pwd)
sdk=${ANDROID_HOME:-$HOME/Android/Sdk}
tools=$sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin
out=$root/dist/lyricon
rm -rf "$out/classes" "$out/generated"
mkdir -p "$out/classes" "$out/generated" "$out/dex"
for file in src/aidl/io/github/proify/lyricon/provider/*.aidl; do
 "$sdk/build-tools/36.1.0/aidl" --lang=java --min_sdk_version=26 -p"$sdk/platforms/android-36/framework.aidl" -Isrc/aidl -o"$out/generated" "$file"
done
mapfile -t sources < <(find src/java "$out/generated" -name '*.java')
javac --release 8 -classpath "$sdk/platforms/android-36/android.jar" -d "$out/classes" "${sources[@]}"
mapfile -t classes < <(find "$out/classes" -name '*.class')
"$sdk/build-tools/36.1.0/d8" --release --min-api 26 --lib "$sdk/platforms/android-36/android.jar" --output "$out/dex" "${classes[@]}"
python3 - "$out" <<'PY'
from pathlib import Path
import sys
out=Path(sys.argv[1]);data=(out/'dex/classes.dex').read_bytes()
(out/'bridge_dex.h').write_text('static const unsigned char bridge_dex[]={'+','.join(str(b) for b in data)+'};\n')
PY
"$tools/aarch64-linux-android26-clang++" -std=c++17 -Oz -flto -shared -static-libstdc++ -fPIC -fno-exceptions -fno-rtti -fno-stack-protector -fvisibility=hidden -ffunction-sections -fdata-sections -I "$root/sdk/include" -I "$out" src/plugin.cpp -landroid -Wl,--gc-sections -Wl,--icf=all -Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384 -o "$out/plugin.so"
"$tools/llvm-strip" --strip-unneeded "$out/plugin.so"
cp manifest.toml README.md CHANGELOG.md UPSTREAM.md icon.png "$out/"
cp catalog.json "$out/metadata.json"
cp licenses/Apache-2.0.txt "$out/LICENSE.txt"
python3 - "$root" <<'PY'
from pathlib import Path
from zipfile import ZipFile,ZIP_DEFLATED
import sys
root=Path(sys.argv[1]);out=root/'dist/official.lyricon-1.1.1-android-arm64-v8a.zip'
with ZipFile(out,'w',ZIP_DEFLATED,compresslevel=9) as z:
 for n in ['manifest.toml','plugin.so','LICENSE.txt','metadata.json','README.md','CHANGELOG.md','UPSTREAM.md','icon.png']:z.write(root/'dist/lyricon'/n,n)
print(out, out.stat().st_size, 'bytes')
PY
