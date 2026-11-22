# 呼吸气球新版固件

适用于 ESP8266 D1 Mini 配套控制板，不是机械臂固件。含控制板的套餐发货前已烧录；只有需要改程序或重刷时才用下面的步骤。接线、供电注意事项和买家问答见[呼吸气球使用说明](../../docs/breathing-balloon.md)。

| 文件 | 内容 |
| --- | --- |
| [platformio.ini](platformio.ini) | ESP8266 Arduino 构建配置，环境 `d1_mini` |
| [src/main.cpp](src/main.cpp) | 热点、网页控制及充抽气逻辑 |
| [include/web_ui.h](include/web_ui.h) | 随固件构建的网页 |

用 VS Code + PlatformIO 打开本目录，选择 `d1_mini` 后编译：

```sh
pio run -e d1_mini
```

需要重刷时，核对板型，断开气泵和阀负载，用 USB 数据线接电脑，再运行 `pio run -e d1_mini -t upload`。网页随固件烧录，不另传。接回负载前请先断电，首次带负载测试须有人看守。

程序来自[现有呼吸气球新版项目](https://gitee.com/zhouhao2528/maker-star-breath-balloon)，此处保留编译所需的源码，不附旧版或二进制固件。源码公开供查看；本仓库未另设软件许可证。构建依赖由 PlatformIO 安装，其许可证和版权声明以各依赖项目为准。
