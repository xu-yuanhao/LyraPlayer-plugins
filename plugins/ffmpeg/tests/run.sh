#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
sdk=${ANDROID_HOME:-$HOME/Android/Sdk}
ndk=$sdk/ndk/28.2.13676358
mkdir -p dist/tests
version=$(python3 -c 'import json;print(json.load(open("plugins/ffmpeg/catalog.json"))["version"])')
"${CXX:-c++}" -std=c++17 -O2 -UNDEBUG "-DLYRA_PLUGIN_VERSION=\"$version\"" \
 -I plugins/ffmpeg/native -I sdk/include \
 -idirafter "$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include" \
 plugins/ffmpeg/tests/screen_throttle_test.cpp plugins/ffmpeg/native/playback/transition_control.cpp \
 -pthread -o dist/tests/screen-throttle
./dist/tests/screen-throttle
