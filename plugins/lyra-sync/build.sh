#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
root=$(cd ../.. && pwd)
sdk=${ANDROID_HOME:-$HOME/Android/Sdk}
ndk=${LYRA_NDK_HOME:-$sdk/ndk/28.2.13676358}
suffix=
case "$(uname -s)" in
  Linux) host=linux-x86_64 ;;
  Darwin) host=darwin-x86_64 ;;
  MINGW*|MSYS*|CYGWIN*) host=windows-x86_64; suffix=.cmd ;;
  *) echo 'Unsupported NDK build host' >&2; exit 1 ;;
esac
bin="$ndk/toolchains/llvm/prebuilt/$host/bin"
stage="$root/dist/lyra-sync"
mkdir -p "$stage"
"$bin/aarch64-linux-android26-clang$suffix" -std=c11 -Oz -fPIC \
  -fvisibility=hidden -shared -Wl,--no-undefined -Wl,-z,max-page-size=16384 \
  -Isrc -I"$root/sdk/include" src/*.c -llog -o "$stage/plugin.so"
"$bin/llvm-strip" --strip-unneeded "$stage/plugin.so"
"${PYTHON:-python3}" - "$root" "$stage" <<'PY'
from pathlib import Path
import json
import sys
import tomllib
from zipfile import ZIP_DEFLATED, ZipFile

root, stage = map(Path, sys.argv[1:])
plugin = Path.cwd()
descriptor = json.loads((plugin / 'catalog.json').read_text(encoding='utf-8'))
with (plugin / 'manifest.toml').open('rb') as stream:
    manifest = tomllib.load(stream)
for key in ('id', 'version', 'abi', 'platform', 'arch', 'entry'):
    if manifest[key] != descriptor[key]:
        raise ValueError(f'Catalog and manifest disagree on {key}')
package = descriptor['package']
with ZipFile(root / 'dist' / package, 'w', ZIP_DEFLATED) as archive:
    archive.write(stage / 'plugin.so', 'plugin.so')
    for name in ('manifest.toml', 'icon.png', 'README.md', 'CHANGELOG.md'):
        archive.write(plugin / name, name)
    archive.write(plugin / 'catalog.json', 'metadata.json')
    archive.write(plugin / 'licenses/MIT.txt', 'LICENSE.txt')
with ZipFile(root / 'dist' / (Path(package).stem + '-source.zip'), 'w', ZIP_DEFLATED) as archive:
    for folder in (plugin, root / 'sdk'):
        for path in sorted(folder.rglob('*')):
            if path.is_file():
                archive.write(path, path.relative_to(root))
print(root / 'dist' / package)
PY
