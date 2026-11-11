#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
LYRA_FFMPEG_FLAVOR=ffmpeg-full bash ffmpeg/build.sh
