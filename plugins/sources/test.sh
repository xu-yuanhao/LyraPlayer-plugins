#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
root=$(cd ../.. && pwd)
work=$root/dist/sources
deps=$work/dependencies
quick=$deps/quickjs-2026-06-04
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
objects=()
for name in quickjs libregexp libunicode cutils dtoa; do
    object=$tmp/$name.o
    cc -O2 -D_GNU_SOURCE '-DCONFIG_VERSION="2026-06-04"' -c "$quick/$name.c" -o "$object"
    objects+=("$object")
done
c++ -std=c++17 -pthread -I "$quick" -I "$deps" -I "$work" -I src src/engine.cpp tests/engine.cpp "${objects[@]}" -lm -o "$tmp/source-tests"
"$tmp/source-tests"
