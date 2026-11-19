#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
root=$(cd ../.. && pwd)
sdk=${ANDROID_HOME:-$HOME/Android/Sdk}
tools=$sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin
out=$root/dist/superlyric-probe
rm -rf "$out/classes"
mkdir -p "$out/classes" "$out/dex"
# Endpoint/Protocol are deliberately absent from the probe classpath: only the
# actual plugin's embedded DEX can supply them. Official AIDL acts as receiver.
javac --release 8 -classpath "$sdk/platforms/android-36/android.jar" -d "$out/classes" tests/DeviceProbe.java
mapfile -t classes < <(find "$out/classes" -name '*.class')
"$sdk/build-tools/36.1.0/d8" --release --min-api 26 --lib "$sdk/platforms/android-36/android.jar" --output "$out/dex" "${classes[@]}"
"$tools/aarch64-linux-android26-clang++" -std=c++17 -shared -fPIC -I "$root/sdk/include" tests/device_probe.cpp -landroid -ldl -o "$out/probe.so"
serial=$1
adb -s "$serial" shell mkdir -p /data/local/tmp/lyra-superlyric-probe
trap 'adb -s "$serial" shell rm -rf /data/local/tmp/lyra-superlyric-probe >/dev/null 2>&1 || true' EXIT
adb -s "$serial" push "$out/dex/classes.dex" "$out/probe.so" "$root/dist/superlyric/plugin.so" /data/local/tmp/lyra-superlyric-probe/ >/dev/null
adb -s "$serial" shell 'CLASSPATH=/data/local/tmp/lyra-superlyric-probe/classes.dex app_process /system/bin org.lyraplayer.superlyrictest.DeviceProbe /data/local/tmp/lyra-superlyric-probe/probe.so /data/local/tmp/lyra-superlyric-probe/plugin.so'
