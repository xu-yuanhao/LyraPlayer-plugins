# 导入音源

在 Lyra Player 的「媒体库」点击「导入音源」。粘贴 HTTPS 脚本链接，或点击「选择文件」选择 UTF-8 JavaScript 文件。

- MusicFree CommonJS 脚本可提供曲目搜索、播放地址和榜单。支持直接 HTTP 请求及 `axios`、`qs` 适配模块。
- LX Music 自定义源可作为具有相应平台标识的歌曲的播放候选。仅提供解析的脚本需搭配可搜索的音源。
- 澜音 Ceru Music v2 支持无需签名和个性化授权的发行包，接入曲目搜索、播放解析、歌词与集合。使用账号、签发授权或需要澜音专用界面的功能暂未接入。
- 支持的榜单会显示在「我的」。详情页可启用、停用或按脚本链接更新音源。
- 网络接口使用 HTTPS。依赖其他模块、私有运行时或未实现能力的脚本可能无法加载。

仅导入你信任的脚本。插件在受限 QuickJS 环境运行脚本，并允许脚本发出 HTTPS 请求。插件包不包含音源脚本。

[MusicFree 协议](https://musicfree.catcat.work/plugin/protocol.html) · [LX Music 脚本说明](https://lxmusic.toside.cn/desktop/custom-source)
