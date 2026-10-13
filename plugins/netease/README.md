# 网易云免费音乐

官方媒体数据插件（`official.netease`，ABI 3，Android arm64-v8a）。安装后直接在现有搜索框搜到网易云免费音乐并播放，不改主程序任何代码。

## 行为

- 在搜索框输入关键词，插件用同一查询搜网易云，**只收录免费曲目**（`fee == 0`），VIP 与付费专辑直接跳过，不会出现在结果里。
- 排序由宿主决定：**本地结果优先显示，在线免费结果追加在后**，和本地资料库、插件市场条目混排，不断层。
- 点一首在线结果：插件即时解析可播放直链（`music.163.com` 外链接口跟随 302，`music.126.net` 改写为 https），由系统下载器取回整文件再解码，播放、进度、歌词、背景律动和本地一致。
- 封面取搜索接口的 `picUrl`，歌词取歌曲歌词接口（含译文合并），点歌时按需加载，不预取。
- 空搜索不触发网络；换关键词时旧查询自动取消。

## 隐私与来源

- 只有在使用搜索框且已安装本插件时，查询关键词才会发往 `music.163.com`（搜索、直链、封面、歌词四个公开接口）。未安装时宿主不把搜索词发给任何服务器。
- 使用网易云公开接口，未获网易授权；接口变更可能导致搜索或播放失败，属预期内波动。
- 音频与图片版权归原权利人，本插件不存储、不转发。

## 构建

```sh
plugins/netease/build.sh
```

需要 Android SDK/NDK 28.2.13676358。产物为 `dist/official.netease-1.0.0-android-arm64-v8a.zip`。解析器另有主机单测（真实抓包样本，不进包）：

```sh
cd plugins/netease/src
g++ -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -I. netease_json.cpp netease_json_test.cpp -o /tmp/netease_json_test
/tmp/netease_json_test ./testdata
```

插件只用公开 C ABI 与 `host->java_vm` 网络，不链宿主私有符号；音频回调不调用插件。
