# Lyra Player 官方插件仓库

统一维护官方插件源码、公共 ABI 头、构建脚本与发布目录，不为每个插件分开建仓库。

- `plugins/<插件名>/`：源码、manifest、依赖构建脚本和第三方声明。
- `sdk/include/plugin/`：固定版本的 C ABI 2 SDK。
- `catalog/index.json`、`index.sig`：软件读取的签名目录；更新目录必须重新签名。
- `scripts/`：目录生成、签名及发布检查。
- `dist/`：本机构建产物，不提交；正式插件包作为 Release 和 GitLab 包附件发布。

GitHub 是主开发仓库，GitLab 为相同提交与发布文件的镜像；镜像并不能保证所有网络环境可达。软件源和镜像地址、公开验证密钥、预算和搜索权重在 Lyra 的配置中。私钥和平台登录凭据不进入仓库或 APK。

## 第一个插件：FFmpeg

`official.ffmpeg`，ABI 2，Android arm64-v8a，替换播放提供者。主程序默认使用系统 MediaExtractor/MediaCodec 解码，插件使用 FFmpeg 7.1.1；保留 mp3、flac、aac、aac_latm、alac、vorbis、opus、ape、wavpack 和现有 PCM 解码器。二者输出统一的音频播放快照与音频特征。插件退出须先 STOP/quiesce，不能在旧工作线程仍活动时卸载。

```sh
plugins/ffmpeg/build.sh
```

需要 Android SDK/NDK 28.2.13676358，以及构建脚本使用的主机工具。默认自动构建 FFmpeg；本机可设置 `LYRA_FFMPEG_PREFIX` 指向已有的同配置构建目录。产物为 `dist/official.ffmpeg-1.0.0-android-arm64-v8a.zip`。

构建的桥接与播放辅助源码来自 LyraPlayer-native 的 c62b234，保持私有编译，不依赖宿主隐藏的 C++ 符号。公共跨库交互只使用 C ABI。平台输出代码编译在插件自身。

## 源码与声明

FFmpeg 使用 LGPL 2.1 或更高版本，完整声明见 `plugins/ffmpeg/licenses`，上游为 https://ffmpeg.org/ 。修改与重建流程位于 `native/third_party/build_ffmpeg.sh`；发布同时提供对应修改后的 FFmpeg 源码附件。插件桥接源码在本仓库，可自行重建并替换。第三方组件沿用各自许可证；本次不变更 Lyra 主仓库的项目许可证。

安装未经信任的原生插件意味着加载进程内代码。签名目录验证发布者、包 SHA-256 验证下载字节，不把原生插件描述成沙箱。

新增官方插件只需增加并列的 `plugins/<名称>/`，其中 `catalog.json` 声明展示信息、兼容性、包名和下载镜像，`manifest.toml` 声明加载信息。通用目录生成器枚举这些描述，读取实际产物计算大小和 SHA-256，不在代码中按插件名分支。生成目录时使用本机私钥；日常同步运行 `scripts/sync.sh`，Git 凭据由本机凭据助手提供。Git 镜像不会自动复制 Release 附件，发布文件须分别上传两端。
