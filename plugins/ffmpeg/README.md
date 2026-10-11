# FFmpeg 播放引擎

为 Lyra Player 提供 FFmpeg 7.1.1 音频解码能力。

## 支持格式

- MP3、FLAC、AAC、AAC LATM、ALAC
- Vorbis、Opus、APE、WavPack
- WAV 与现有 PCM 格式

## 使用方式

安装后通过公开播放接口接管解码，继续使用 Lyra 的音频输出时钟、歌词、进度记忆和背景律动。

卸载后恢复系统解码。系统支持的格式因设备而异。

## 作者与开源

插件维护：Xu YuanHao。FFmpeg：FFmpeg contributors。

FFmpeg 使用 LGPL-2.1-or-later；许可证随插件附带，完整插件源码与对应 FFmpeg 源码公开提供。

[插件源码](https://github.com/xu-yuanhao/LyraPlayer-plugins/tree/main/plugins/ffmpeg)

[FFmpeg 官网](https://ffmpeg.org/)

## 图标

使用 FFmpeg 官网的图标，来源：https://ffmpeg.org/favicon.ico 。图标归 FFmpeg 项目所有，不表示 FFmpeg 项目为本插件背书。
