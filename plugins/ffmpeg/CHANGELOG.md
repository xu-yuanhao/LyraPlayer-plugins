## 1.4.1
- Track changes now use a plain linear crossfade; the adaptive analysis, tempo
  matching, stem handoff and the transition-stems dependency are removed.
- No analysis caches are written anymore.

# 1.4.0

- 增加 AC-3 与 E-AC-3（Dolby Digital / Dolby Digital Plus、含 Atmos 音轨）解码，之前这类 m4a 只能退回系统解码器。
- 配套启用 ac3 解析器；多声道按现有立体声下混输出，其余格式与播放路径不变。

# 1.3.0

- 将 Stem 模型拆为可选的 official.transition-stems 插件；本包不含模型。
- 包含 1.3.0-dev.5 的坏帧恢复、FLAC 识别和过渡修复。
- 修正下载描述，确保版本、包名及下载文件一致。

# 1.3.0-dev.5

- Recover after invalid encoded packets instead of ending a playable track at the first damaged frame.

# 1.3.0-dev.4

- Recognize FLAC after a large leading ID3 tag from fixed-size headers and select the FLAC demuxer while retaining metadata.

# 1.3.0-dev.3

- Introduce incoming accompaniment immediately instead of suppressing midrange and gating bass until mid-transition. Retain separate vocal handoff and peak protection.

# 1.3.0-dev.2（过渡硬切修复）

- 修复人声退出点为零时，旧歌人声第一帧被截断。
- 两段人声包络始终完整落在最终过渡时长内，覆盖短过渡与乐句调整。
- 只在两侧都有对齐且完整覆盖的分轨时启用 Stem；缓存缺失或速度不匹配时保留完整自适应过渡。
- 增加真实混音器的五种边界场景与首尾音频样本回归。

# 1.3.0-dev.1（本地分轨试用版）

- 后台流式整曲结构与真实人声分析，持久化小索引和有界分轨 PCM 缓存。
- 打包独立 Spleeter/ncnn 依赖，实际分离人声与伴奏，交接时分别控制包络。
- 未准备好的曲目沿用自适应过渡；不把推理放进音频输出路径。
- 保留混合采样率切换后 seek 的输出时钟及速度重置修复。

# 1.2.0-dev.3（本地试听版）

- 通过可选消息扩展接入有界首尾静音分析，保留弱音/混响余量。
- 首曲与过渡目标跳过确认的片头静音，自动交接参考有效音乐结尾。
- 保留原曲进度、歌词坐标与显式 seek；短全静音曲不跳成空曲。
- 同一解码器扫描并重置，不增加第三个解码器。

# 1.2.0-dev.2（本地试听版）

- 修复 AAudio 重复跳转后时间戳落后，导致进度停住、自动和手动过渡失效。
- 跳转时重建输出流以清除帧计数偏移；连续切歌仍复用同一个输出流。
- 增加真实 AAudio 连续 60 轮跳转与切歌静音回归。

# 1.2.0-dev.1（本地试听版）

- 通过接口 23 接入自动、手动切歌的节拍匹配和频段交接。
- 复用单个 AAudio 输出，映射变速后的源时间，支持准备中 seek、连续切歌和安全卸载。
- 不变更现有解码器列表。

# 1.1.1

- seek 请求立即暂停旧输出，等待定位期间冻结音频时钟，避免歌词和进度提前跳转。
- 曲尾使用统一播放时钟判断最后一个音频帧是否已呈现，避免等待下一次硬件时间戳。
- 解码格式和 ABI 3 保持不变。

# 1.1.0

采用当前 ABI 3 插件契约；仅支持当前内测宿主，不提供旧 ABI 适配。解码格式保持不变。

# 1.0.1

- 新增作者、官方 FFmpeg 图标、离线 Markdown 说明与更新记录。
- 解码器及播放实现保持一致。
