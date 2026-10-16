#!/usr/bin/env python3
"""Check release metadata against package bytes and enforce optional model packaging."""
from pathlib import Path
import hashlib,json,tomllib,zipfile
root=Path(__file__).resolve().parents[1]
for name in ('ffmpeg','transition-stems'):
 descriptor=json.loads((root/f'plugins/{name}/catalog.json').read_text())
 path=root/'dist'/descriptor['package']
 with zipfile.ZipFile(path) as archive:
  assert archive.testzip() is None
  manifest=tomllib.loads(archive.read('manifest.toml').decode())
  for key in ('id','version','entry'):assert manifest[key]==descriptor[key],(name,key)
  assert descriptor['package']==f"{manifest['id']}-{manifest['version']}-android-arm64-v8a.zip"
  assert all(url.endswith('/'+descriptor['package']) for url in descriptor['urls'])
  if name=='ffmpeg':
   assert path.stat().st_size<1048576
   assert not any(n.startswith(('stems/','models/')) for n in archive.namelist())
  else:
   for asset in ('worker','models/vocals.param','models/vocals.bin'):assert asset in archive.namelist()
   assert hashlib.sha256(archive.read('models/vocals.bin')).hexdigest()=='401c9ca0845137852eebcd3961c5754fb7e502d995cc81d15ee5812620817bf9'
 print(manifest['id'],manifest['version'],path.stat().st_size,hashlib.sha256(path.read_bytes()).hexdigest())
