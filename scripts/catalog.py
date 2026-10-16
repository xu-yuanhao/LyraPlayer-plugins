#!/usr/bin/env python3
"""Generate the signed directory from plugin descriptors, never from repository-specific UI rules."""
import hashlib,json,os,subprocess,base64
from pathlib import Path
root=Path(__file__).resolve().parents[1]
entries=[]
for descriptor in sorted((root/'plugins').glob('*/catalog.json')):
    entry=json.loads(descriptor.read_text());name=entry.pop('package')
    if '/' in name or '\\' in name:raise ValueError('Package must be a dist filename')
    archive=root/'dist'/name
    for field in ('readme','changelog'):
        value=entry.get(field,'')
        if value and '\n' not in value:
            path=descriptor.parent/value
            if path.parent.resolve()!=descriptor.parent.resolve():raise ValueError('Markdown must be a package file')
            entry[field]=path.read_text()
    icon=entry.get('icon','')
    if icon and not icon.startswith('data:'):
        path=descriptor.parent/icon
        if path.parent.resolve()!=descriptor.parent.resolve() or path.suffix not in ('.png','.webp'):raise ValueError('Icon must be PNG/WebP in package')
        data=path.read_bytes()
        if len(data)>98304:raise ValueError('Icon too large')
        entry['icon']='data:image/'+path.suffix[1:]+';base64,'+base64.b64encode(data).decode()
    entry.update(bytes=archive.stat().st_size,sha256=hashlib.sha256(archive.read_bytes()).hexdigest())
    entries.append(entry)
index={'schema':1,'plugins':entries}
directory=root/'catalog/v2';directory.mkdir(parents=True,exist_ok=True)
path=directory/'index.json';path.write_text(json.dumps(index,ensure_ascii=False,sort_keys=True,indent=2)+'\n')
key=Path(os.environ.get('LYRA_PLUGIN_SIGNING_KEY',str(Path.home()/'.local/share/lyra-player/keys/plugin-catalog-v2.pem')))
subprocess.run(['openssl','dgst','-sha256','-sign',str(key),'-out',str(directory/'index.sig'),str(path)],check=True)
print(path)
