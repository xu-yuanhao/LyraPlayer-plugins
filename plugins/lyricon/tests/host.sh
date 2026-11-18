#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
javac --release 8 -d "$out" src/java/org/lyraplayer/lyricon/Document.java tests/host/DocumentTest.java
java -cp "$out" org.lyraplayer.lyricon.DocumentTest
