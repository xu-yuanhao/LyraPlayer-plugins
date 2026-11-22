#!/usr/bin/env python3
"""Publish plugin packages to the Gitee mirror and add its URLs to the catalog.

Reads the gitee CLI token, creates a release per plugin tag when missing, and
uploads each package listed in the catalog when the release does not already
carry it. Writes plugins/*/catalog.json 'urls' entries for the Gitee mirror,
then regenerates and re-signs catalog/v2 (via scripts/catalog.py).
"""
import hashlib, json, subprocess, sys, time
from pathlib import Path

root = Path(__file__).resolve().parents[1]
API = 'https://gitee.com/api/v5'
OWNER_REPO = 'please-call-me-xu-zong/LyraPlayer-plugins'
token = Path.home().glob('.config/gitee/credentials.yml')
token = subprocess.run(['grep','-oP',r'token:\s*\K.*',str(next(token))],capture_output=True,text=True).stdout.strip()

def api(path, data=None, is_json=False, retries=6):
    for attempt in range(retries):
        sep='&' if '?' in path else '?'
        cmd = ['curl','-s','--max-time','90',API+path+sep+'access_token='+token]
        if data is not None:
            cmd += ['-X','POST']
            if is_json:
                cmd += ['-H','Content-Type: application/json','-d',json.dumps(data)]
            else:
                for k,v in data.items(): cmd += ['--data-urlencode',f'{k}={v}']
        cmd += ['-w','\n%{http_code}']
        r = subprocess.run(cmd,capture_output=True,text=True,timeout=120)
        body, _, code = r.stdout.rpartition('\n')
        if code == '200' or (data is not None and code in ('201','200')):
            return code, json.loads(body) if body.strip() else {}
        if attempt < retries-1: time.sleep(3)
    return code, {}

def release_for(tag, commit):
    code, rel = api(f'/repos/{OWNER_REPO}/releases/tags/{tag}')
    if code == '200' and isinstance(rel, dict) and rel.get('id'): return rel
    code, rel = api(f'/repos/{OWNER_REPO}/releases', {'tag_name':tag,'name':tag,'body':'LyraPlayer 插件包镜像发布。','target_commitish':commit})
    if code != '201': print(f'release create failed {tag}: {code}', file=sys.stderr); sys.exit(1)
    return rel

def attach(release, path):
    name = path.name
    existing = {a.get("name") for a in (release.get("assets") or [])}
    if name in existing:
        print(f'  已存在 {name}')
        return True
    r = subprocess.run(['curl','-s','--max-time','300','-X','POST',
        f'{API}/repos/{OWNER_REPO}/releases/{release["id"]}/attach_files?access_token={token}',
        '-F',f'file=@{path}','-w','\n%{http_code}'],capture_output=True,text=True,timeout=320)
    body, _, code = r.stdout.rpartition('\n')
    if code != '201':
        print(f'  上传失败 {name}: {code} {body[:200]}', file=sys.stderr); return False
    print(f'  已上传 {name}')
    return True

def catalog_urls(descriptor, tag, package):
    return f'https://gitee.com/{OWNER_REPO}/releases/download/{tag}/{package}'

descriptors = sorted((root/'plugins').glob('*/catalog.json'))
changed = False
for descriptor in descriptors:
    entry = json.loads(descriptor.read_text())
    tag = subprocess.run(['git','-C',str(root),'describe','--tags','--exact-match','HEAD']
        if False else ['git','-C',str(root),'rev-list','-n','1',descriptor.parent],capture_output=True,text=True).stdout.strip()
    # tag comes from the descriptor's version + package id naming used by this repo
    pkg_id, version = entry['id'], entry['version']
    tag = f'{pkg_id.removeprefix("official.")}-v{version}'
    package = entry['package']
    archive = root/'dist'/package
    if not archive.exists():
        print(f'跳过（无包） {pkg_id} {version}', file=sys.stderr); continue
    # find the git tag object for this plugin+version to get a valid commit
    tag_ref = subprocess.run(['git','-C',str(root),'tag','-l',tag],capture_output=True,text=True).stdout.strip().splitlines()
    if not tag_ref:
        print(f'跳过（无 tag） {pkg_id} {version} (tag {tag})', file=sys.stderr); continue
    commit = subprocess.run(['git','-C',str(root),'rev-list','-n','1',tag],capture_output=True,text=True).stdout.strip()
    print(f'{pkg_id} {version} tag={tag}')
    rel = release_for(tag, commit)
    if not attach(rel, archive): sys.exit(1)
    url = catalog_urls(descriptor, tag, package)
    if url not in entry.get('urls',[]):
        entry.setdefault('urls',[]).append(url)
        descriptor.write_text(json.dumps(entry,ensure_ascii=False,indent=2)+'\n')
        changed = True
        print(f'  已加入 URL {url}')

if changed:
    subprocess.run([sys.executable,str(root/'scripts/catalog.py')],check=True)
    print('catalog 已重新签名')
else:
    print('无需变更')
