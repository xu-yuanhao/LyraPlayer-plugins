# SuperLyric 歌词连接

将 Lyra 当前歌曲的主歌词、逐字时间、译文和注音发布到 SuperLyric 4.x 服务。它是独立插件，与 Flyme、Lyricon 插件互不替代，不内置歌词显示端。

需要已安装并激活 SuperLyric，以及带有 `superlyricapi` API 声明的新版 Lyra。旧版 Lyra 没有该声明，服务端会拒绝；只更新插件无法补齐宿主声明。

安装启用后开始播放即可。整首歌词按歌曲/内容变化发送，不逐帧重复发送。暂停、恢复和拖动由 SuperLyric 按系统 MediaSession 同步；停止或无歌词时清除，服务重启后重新发布。通信在插件工作线程，不阻塞宿主绘制。

仅支持 4.x 的 `super_lyric.v4` 协议，旧 3.x 服务不接入。服务不可用或系统限制服务查询时自动等待重试，不绕过系统限制。不获取网络歌词、不写歌词日志。

公开接口：https://github.com/HChenX/SuperLyricApi

构建：`plugins/superlyric/build.sh`。使用现有 Lyra ABI 3、系统 Binder 和插件内嵌 DEX；宿主只需 Manifest API 标记，无额外播放状态或依赖库。

验证范围：已用 Android 设备运行发布插件的真实内嵌 DEX/Binder 协议测试；未安装 SuperLyric 的设备不能验收最终状态栏显示，需在实际模块环境中确认。
