#!/usr/bin/env python3
"""Build the official catalog. Signing key stays in the user's private local vault."""
import hashlib,json,os,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1]
archive=root/'dist/official.ffmpeg-1.0.0-android-arm64-v8a.zip'
index={'schema':1,'plugins':[{'id':'official.ffmpeg','title':'FFmpeg 播放引擎','version':'1.0.0','abi':2,'platform':'android','arch':'arm64-v8a','capability':'playback','description':'扩展音频格式支持，提供 APE、WavPack、ALAC 等解码能力。安装后使用 FFmpeg 播放，保留统一歌词、进度和背景律动。','tags':'ffmpeg 解码器 引擎 播放 ape wavpack alac flac mp3 aac vorbis opus 无损','bytes':archive.stat().st_size,'sha256':hashlib.sha256(archive.read_bytes()).hexdigest(),'urls':['https://github.com/xu-yuanhao/LyraPlayer-plugins/releases/download/ffmpeg-v1.0.0/'+archive.name,'https://gitlab.com/api/v4/projects/87466640/packages/generic/official.ffmpeg/1.0.0/'+archive.name]}]}
path=root/'catalog/index.json';path.write_text(json.dumps(index,ensure_ascii=False,sort_keys=True,indent=2)+'\n')
key=Path(os.environ.get('LYRA_PLUGIN_SIGNING_KEY',str(Path.home()/'.local/share/lyra-player/keys/plugin-catalog.pem')))
subprocess.run(['openssl','dgst','-sha256','-sign',str(key),'-out',str(root/'catalog/index.sig'),str(path)],check=True)
print(path)
