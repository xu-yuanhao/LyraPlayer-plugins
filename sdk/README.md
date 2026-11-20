# 公共 SDK（ABI 3）

头文件来自并列 `LyraPlayer-native/native/plugin` 的公共接口。宿主能力按完整 size/version 查询；查询失败代表不支持，不使用私有符号回退。接口分配以宿主 `architecture/interface-ledger.json` 为准。

- `settings_contracts.h`：通用设置页、卡片、开关、滑块、确认。INFO 可用 ENTRY_PARENT 指定宿主父页；未知父页不回落到首页。没有通用图标行、文本输入事务。
- `service_call_contracts.h`（28）：在帧线程按服务句柄和消息接口 ID 同步调用。使用 `lyra_service_call`，由注册器建立被调用方所有权并校验返回；不保留插件函数表。嵌套 MESSAGE 只用于本进程同步调用，不能序列化、落盘或延迟执行。返回字段借用到下一次调用前，插件须立即复制需要的数据。
- `media_actions_contracts.h`（27）：有效媒体上下文的 collection 请求 ID 与 row 可用于追加或下次播放。接收时复制媒体值，重复歌曲获得独立队列出现身份。请求 ID/row 不能保存为持久身份；该接口不等于在线歌单持久化能力。

`personal_contracts.h`（21）的 ADD 可用 PROVIDER/UID 保存来源媒体引用，不能与 TRACK 混用；原本地 ID 调用仍受支持。LIST 的 tracks 支持原数字字符串及 provider/uid 对象，根级 provider 声明数字条目的来源。引用去重按完整身份，提供者缺失时保留。该存储能力不代表通用在线歌单界面已交付。

提供者集合呈现、不可用曲目呈现、统一媒体菜单、队列跨进程恢复和带所有者的文件选择事务仍在宿主整改中，不视为当前 SDK 已交付能力。

Android 方法使用前必须精确匹配 `android-platform-v9.json`；Activity 引用不是永久 Application 引用。所有工作线程、JNI 引用、fd 和延迟任务必须在卸载前回收。原生插件是进程内代码，不是沙箱。
