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

## 本地过渡试听版 1.2.0-dev.3

需要支持 `LYRA_INTERFACE_AUDIO_TRANSITION`（接口 23）的宿主。过渡参数由宿主 `audio_transition` 配置传入；自动和手动切歌共用一套 native 交接逻辑。可靠节拍时保调对拍，节拍证据不足时保留完整频段混合，不强行变速。旧宿主不提供配置时默认关闭，维持普通播放。

Signalsmith Stretch 与 Signalsmith Linear 为 MIT 许可，文本随包放在 `licenses/`。`test.sh <serial>` 用真实 FFmpeg 解码和静音可测输出覆盖过渡、时钟、seek、快速切换及插件导出入口。此版本仅用于本地体验，尚未发布到在线目录。

`test-hardware.sh <serial>` 通过真实 AAudio 输出静音，分别在 48kHz 和 44.1kHz 连续验证 60 轮跳转与切歌。跳转重建输出流，切歌复用该流，验证时钟持续就绪和欠载计数。

试听版 3 增加可选首尾静音分析：扫描上限、窗口、双声道 RMS/峰值阈值与保留余量均由宿主配置。原始时长与歌词时间不变，显式 seek 不强制跳过静音。

## 1.3.0-dev.1 分轨试用版

包含真实 Spleeter 人声/伴奏分离依赖。后台整曲分析只保存曲线索引和有界 PCM 缓存；首次准备需要时间和 CPU。推理运行在独立原生 helper 中，不进入播放进程的输出混音路径。已有分轨缓存时分别交接人声与伴奏，未准备或模型失败时沿用现有策略。主 APK 不包含模型。不是鼓/贝斯四轨分离。


## 独立可选 Stem 插件

本包只含播放引擎；无需模型即可解码和执行自适应过渡。需要真实人声／伴奏分析时，另装 `official.transition-stems` 并重启播放器。两包可分别升级、卸载；卸载 Stem 后重启恢复非神经网络过渡。
