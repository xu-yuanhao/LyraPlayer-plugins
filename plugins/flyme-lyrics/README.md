# Flyme 状态栏歌词

在支持状态栏歌词的 Flyme 设备上，将 Lyra 当前演唱的歌词显示在系统状态栏。安装并启用插件即可使用；请同时开启系统中的状态栏歌词选项，并允许 Lyra 的播放通知。

- 使用魅族官方公开的通知 ticker 适配方案，不需要 Root、悬浮窗或通知读取授权。
- 自动探测设备能力；不支持该扩展的设备上不启动定时器和通知线程。
- 复用当前播放通知，保留原有封面、按钮、MediaSession 和通知渠道，不增加第二条通知。
- 跟随播放进度和跳转，暂停、停止、间奏、无时间轴歌词和切歌等待时清除歌词；对唱重叠时显示最近开始的当前主句。
- 后台同步通过系统事件循环运行，不依赖界面绘制，不要求持续绘制或提高屏幕刷新率。
- 卸载/禁用插件时清除由插件设置的歌词标志，不删除播放通知。

需要支持 ABI 3 歌词与播放公共接口的 Lyra Player、Android 8 或以上。只调用已有插件 SDK 与系统接口，不修改宿主或增加宿主 APK 体积。歌词不联网、不上传、不写入日志。不支持该功能的设备无需安装。

## 兼容性

按 Flyme 官方文档探测 `FLAG_ALWAYS_SHOW_TICKER` 与 `FLAG_ONLY_UPDATE_TICKER`；接口缺失或被系统限制时保持不启用。不同 Flyme 机型、系统设置和版本可能影响显示。首版尚无 Flyme 真机视觉验收，欢迎反馈机型和系统版本。

官方适配说明：https://open.flyme.cn/docs?id=239

## 构建

在仓库根目录运行 `plugins/flyme-lyrics/build.sh`。使用 Android SDK 36 / NDK 28.2.13676358；Java 系统通知桥编译为 DEX 并内嵌在插件自身，通过 Android 8 起公开的 InMemoryDexClassLoader 加载，无额外 APK、宿主 Java 类或私有符号依赖。

测试：`python3 plugins/flyme-lyrics/tests/run.py`。
