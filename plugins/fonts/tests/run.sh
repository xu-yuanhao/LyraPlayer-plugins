#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
root=$(cd ../.. && pwd)
native=${LYRA_NATIVE_ROOT:-$(cd "$root/../LyraPlayer-native" && pwd)}
python3 generate_strings.py
mkdir -p "$root/dist/fonts-tests"
c++ -std=c++17 -O1 -g -UNDEBUG -I "$native/native" -I "$root/sdk/include" -I src tests/integration.cpp src/font_catalog.cpp src/font_import.cpp src/font_metadata.cpp "$native/out/architecture-host/liblyra_core.a" "$native/out/architecture-host/liblyra_lyrics.a" -ldl -lz -pthread -o "$root/dist/fonts-tests/integration"
"$root/dist/fonts-tests/integration" "$native/config/defaults/ui.toml" "$native/config/defaults/zh-Hans.toml"
