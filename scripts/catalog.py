#!/usr/bin/env python3
"""Generate the signed directory from plugin descriptors, never from repository-specific UI rules."""
import hashlib,json,os,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1]
entries=[]
for descriptor in sorted((root/'plugins').glob('*/catalog.json')):
    entry=json.loads(descriptor.read_text());name=entry.pop('package')
    if '/' in name or '\\' in name:raise ValueError('Package must be a dist filename')
    archive=root/'dist'/name
    entry.update(bytes=archive.stat().st_size,sha256=hashlib.sha256(archive.read_bytes()).hexdigest())
    entries.append(entry)
index={'schema':1,'plugins':entries}
path=root/'catalog/index.json';path.write_text(json.dumps(index,ensure_ascii=False,sort_keys=True,indent=2)+'\n')
key=Path(os.environ.get('LYRA_PLUGIN_SIGNING_KEY',str(Path.home()/'.local/share/lyra-player/keys/plugin-catalog.pem')))
subprocess.run(['openssl','dgst','-sha256','-sign',str(key),'-out',str(root/'catalog/index.sig'),str(path)],check=True)
print(path)
