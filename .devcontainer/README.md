# 开发容器

此容器固定使用官方 `espressif/idf:v6.1` 发布标签，对应 ESP-IDF 6.1.0。官方文档区分了发布标签 `vX.Y`、跟随发布分支的 `release-vX.Y` 和跟随主分支的 `latest`，见 [IDF Docker 镜像说明](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/tools/idf-docker-image.html#tags)。

本项目开发机的 SDK 对应官方 [v6.1 发布](https://github.com/espressif/esp-idf/releases/tag/v6.1)，提交为 `fff9895c82d744c7237be8847347bdd1b07c6643`。项目的 `main/CMakeLists.txt` 会校验并观测 I2S 写入实现；更新容器中的 SDK 前须重新检查该钩子。已核对官方 v6.1 的 `components/esp_driver_i2s/i2s_common.c` 与开发机文件一致，统一换行后的 SHA-256 为 `7bfc9633a327eb2b04b9c0b9d8c97bc387215ed32e9c3a19a0ca84f911b0a362`。

首次打开前安装 Docker 和 VS Code Dev Containers 扩展，然后使用“在容器中重新打开”。SDK 就绪后，按[首次固件配置说明](../docs/operations.md#新克隆与默认插话固件)选择 `esp32s3` 并填写本机配置。容器使用 Linux shell，文档中的 Windows 路径和反斜杠需换为实际工作目录与正斜杠。设备串口能否在容器中使用取决于宿主机的设备转发，Windows 用户可继续在宿主机 ESP-IDF PowerShell 中烧录。

当前版本已完成 SDK 版本与上述源码的一致性检查；容器首次实际使用时，仍需验证启动和目标固件构建。
