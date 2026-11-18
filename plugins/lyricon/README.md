# Lyricon 歌词连接

将 Lyra 当前歌曲的主歌词、逐字时间、译文和注音发送给 Lyricon（词幕）。这是独立的 Lyra 插件，和 Flyme 状态栏歌词互不替代，不修改宿主 APK。

## 使用

1. 安装并按 Lyricon 官方指南激活词幕及其中心服务。
2. 在 Lyra 插件市场安装本插件，然后播放有时间轴歌词的歌曲。
3. 在词幕中配置歌词显示。没有歌词、停止或切歌等待时会清除上一首的内容。

插件不自行绘制状态栏歌词、不联网获取歌词、不申请 Root 或悬浮窗权限。显示端所需的系统/LSPosed 配置由 Lyricon 自身负责。

Android 8.1 或以上运行；Android 8.0 保持停用。支持当前 ABI 3 的 Lyra。连接断开后自动重试，中心服务重启后重新注册并补发当前歌曲。进度使用 Lyricon 的共享内存接口同步实际播放进度，跳转使用其 seek 接口，不依赖宿主持续绘制。

## 与官方 Provider 模块的区别

本插件加载在 Lyra 进程中，不是另一个 Android APK，因此不会给宿主 Manifest 添加 `lyricon_module` 元数据。当前 Lyricon 中心服务接受运行时 Binder 注册，元数据主要用于应用中的模块发现列表；本插件不会作为一个独立 APK 出现在该列表中。歌词由 Lyra 的真实包名和进程注册，不伪装其他应用。后续 Lyricon 若修改接入协议或增加 Manifest 强制校验，需要重新评估兼容性。

当前已验证协议互通、连接恢复、资源清理和 Android 加载；实际 Lyricon 状态栏视觉仍由用户验收。

## 开发

- 官方项目：https://github.com/tomakino/lyricon
- Provider 文档：https://tomakino.github.io/lyricon/zh-cn/developer/provider/quick-start
- 构建：`plugins/lyricon/build.sh`
- 设备协议测试：`plugins/lyricon/tests/device.sh <ADB序列号>`

使用 Android SDK/NDK 的 Java、AIDL、Binder、SharedMemory、JSON 和 ZLIB 能力，不引入 Kotlin、AndroidX 或 Gradle。原生插件仅通过现有公共接口取得宿主数据；系统通信在独立线程进行。源代码、协议来源与许可证随包提供。
