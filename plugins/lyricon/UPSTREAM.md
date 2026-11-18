# 协议来源

Lyricon / Provider Bridge，Copyright 2026 Proify, Tomakino，Apache-2.0。

来源：https://github.com/tomakino/lyricon
固定复核提交：c93da0eef8b7aee015d2670b48e9e262fcf72cd1

`src/aidl` 保留官方接口的包名、方法顺序与类型；只移除了未使用的 Song import。`Protocol.java` 是基于官方注册、连接及 Player 通道实现的 Java 适配；使用系统 JSON/ZLIB 与共享内存，未复制 Kotlin/AndroidX 依赖。

Lyra 插件的导出和 Java 加载部分 Copyright 2026 Xu YuanHao，按 Apache-2.0 发布。完整源代码在本仓库，随插件公开。
