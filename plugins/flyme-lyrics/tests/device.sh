#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
root=$(cd ../.. && pwd)
sdk=${ANDROID_HOME:-$HOME/Android/Sdk}
tools=$sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin
out=$root/dist/flyme-probe
rm -rf "$out/classes"
mkdir -p "$out/classes" "$out/dex"
javac --release 8 -classpath "$sdk/platforms/android-36/android.jar" -d "$out/classes" tests/DeviceProbe.java
"$sdk/build-tools/36.1.0/d8" --release --min-api 26 --lib "$sdk/platforms/android-36/android.jar" --output "$out/dex" "$out"/classes/org/lyraplayer/flymelyrics/*.class
"$tools/aarch64-linux-android26-clang++" -std=c++17 -shared -fPIC -I "$root/sdk/include" tests/device_probe.cpp -landroid -ldl -o "$out/probe.so"
serial=$1
adb -s "$serial" shell mkdir -p /data/local/tmp/lyra-flyme-probe
adb -s "$serial" push "$out/dex/classes.dex" "$out/probe.so" "$root/dist/flyme-lyrics/plugin.so" /data/local/tmp/lyra-flyme-probe/ >/dev/null
adb -s "$serial" shell 'CLASSPATH=/data/local/tmp/lyra-flyme-probe/classes.dex app_process /system/bin org.lyraplayer.flymelyrics.DeviceProbe /data/local/tmp/lyra-flyme-probe/probe.so /data/local/tmp/lyra-flyme-probe/plugin.so'
adb -s "$serial" shell rm -rf /data/local/tmp/lyra-flyme-probe
