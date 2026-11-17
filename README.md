# 语音助手

ESP32-S3 设备固件与本地 Python 语音服务，采用可独立选择的 ASR → LLM → TTS 流水线。当前本地 `realtime_default` 组合为豆包 ASR 2.0 → DeepSeek → 曼波 TTS；配置模板仍保留火山 TTS。曼波启动与验证见[服务端说明](server/README.md#本地曼波-tts)。设备固定请求 `realtime_default`，通过 GPIO7／GPIO3 双按钮操作，就绪后自动监听并启用可用的 D2 插话，日常无需串口输入。

运行环境：固件使用 **ESP-IDF 6.1**；Python 语音服务需要 **Python 3.11 或更新版本，推荐已验证的 Python 3.12**。曼波引擎使用它自己附带的 Python 环境，安装步骤见[服务端说明](server/README.md#环境与依赖)。

**从 GitHub 下载后想使用曼波音色？** 本仓库提供曼波/GPT-SoVITS 的 TTS 接口适配代码，**不包含曼波引擎、模型权重和参考音频**，也不会在 ESP32 上运行模型。请先按[曼波使用说明](server/README.md#本地曼波-tts)另行安装引擎，修改自己的参考音频路径，并将设备组合的 TTS 切换为 `mambo_local`。运行时电脑上需要同时保持 **9880 曼波引擎**与 **8000 语音服务**；ESP32 连接 8000。公开模板的 `realtime_default` 使用火山 TTS，不会自动改成作者本机的曼波组合。

从[文档索引](docs/README.md)开始：

**第一次改模型，先用中文配置向导**（在项目根目录的 PowerShell 中运行）：

```powershell
cd C:\esp-project\voice_assistant
.\.venv-voice\Scripts\python.exe server\configure_voice.py
```

按编号选择语音识别 ASR、聊天模型 LLM、播报声音 TTS，再填写相应密钥并检查。向导默认编辑设备实际请求的 `realtime_default`，保存前校验、保存时备份；更改后重启语音服务。字段说明与例子见[新手配置](server/README.md#新手配置)。缺少 Python 环境时先按[环境与依赖](server/README.md#环境与依赖)安装。

| 要做什么 | 文档 |
| --- | --- |
| 阅读完整开发说明、测试方案、风险和结项复盘 | [项目开发文档](docs/project_development.md) |
| 看已完成内容、项目收尾状态和已知限制 | [当前进度](docs/realtime_voice_progress.md) |
| 启动服务、构建烧录、双按钮操作、自动恢复、日志 | [操作指南](docs/operations.md) |
| 选择模型、音色、语速、密钥及 Wi-Fi | [新手配置](server/README.md#新手配置) |
| 安装本地曼波、准备模型、启动两个服务及排错 | [曼波使用说明](server/README.md#本地曼波-tts) |
| 新闻频道、热搜、ShowAPI AppKey 及授权范围 | [新闻 API 配置](server/README.md#showapi-新闻-api) |
| 理解架构和 P5 规划 | [总体规划](docs/realtime_voice_plan.md) |
| 开发服务端或修改通信 | [服务端说明](server/README.md) · [设备协议](docs/voice_protocol.md) |
| 使用固定 ESP-IDF 版本的开发容器 | [开发容器说明](.devcontainer/README.md) |
| 查以前的故障与验证证据 | [历史归档](docs/archive/README.md) |

当前开发机的固件操作使用根 `sdkconfig` 和 `build\barge-in-test`，由用户执行构建、烧录和串口监视。新克隆没有本地 `sdkconfig` 时，首次配置从 `sdkconfig.defaults` 生成，并默认启用 AEC/D2 插话；完整首次配置步骤见[操作指南](docs/operations.md#新克隆与默认插话固件)。

新克隆首次生成配置时，`sdkconfig.defaults` 默认针对 **ESP32-S3 N16R8（16 MB Flash / 8 MB Octal PSRAM）**启用本地 VAD、AEC 与 D2 插话。首次配置在 ESP-IDF PowerShell 中运行 `idf.py -B build\barge-in-test set-target esp32s3`，再运行 `idf.py -B build\barge-in-test menuconfig` 填自己的 Wi-Fi、电脑服务 IP 和实际接线，然后构建。默认服务器 IP `192.168.1.10` 是示例地址。AEC/插话需在实际麦克风、扬声器和房间环境中验证；半双工回退方法见[操作指南](docs/operations.md#新克隆与默认插话固件)。本次没有修改你的本机 `sdkconfig`；已有项目重新执行 `set-target` 前应备份它，因为该操作会重新生成配置。

```text
voice_assistant/
├── main/                 ESP-IDF 固件组件
│   ├── main.cpp          程序入口
│   ├── app/              应用初始化与状态
│   │   └── board_pins.hpp 外设接线 GPIO 的统一定义
│   ├── audio/            麦克风、扬声器、音频任务和 Opus
│   ├── display/          屏幕驱动与界面
│   ├── fonts/            界面字体资源
│   └── network/          Wi-Fi、WebSocket 和音频帧协议
├── server/               Python HTTP / WebSocket 语音服务
│   ├── core/             配置、能力、对话回合与流水线
│   ├── configure_voice.py 中文配置向导（不启动服务）
│   ├── providers/        ASR / LLM / TTS 厂商适配器
│   ├── transport/        HTTP / WebSocket 与统一音频协议
│   ├── audio/            PCM 格式与有状态 Opus 编解码
│   ├── config/           模型组合、凭证引用与示例
│   ├── data/             调试录音
│   └── tests/            服务端测试
├── docs/                 当前文档入口；archive/ 保存阶段记录
├── tools/                PC 语音客户端与字体生成工具
├── partitions.csv        ESP32 Flash 分区表
└── sdkconfig.defaults    固件默认配置
```

## 测试与维护

已有 Python 环境并安装服务依赖后，可在项目根运行离线回归：

```powershell
cd C:\esp-project\voice_assistant
.\.venv-voice\Scripts\python.exe -m unittest discover -s server/tests -t server -v
```

测试层级和用户实测结果见[测试方案](docs/project_development.md#10-测试方案)。维护时遵循 [AGENTS.md](AGENTS.md)：按模块职责修改，同步源码/配置/文档，保持密钥、构建和运行数据不入库；固件构建、烧录和串口操作由用户执行。

## 许可与第三方资源

本项目自有代码采用 [MIT 许可证](LICENSE)，版权归 `yizhiwx12` 所有。使用、修改或分发时请保留许可证和版权声明。

`main/fonts/lv_font_chinese_16.c` 是 Noto Sans SC 字体的转换资源，继续采用 [SIL Open Font License 1.1](main/fonts/LICENSE-OFL.txt)，发布时须保留[字体来源说明](main/fonts/README.md)和对应版权声明。ESP-IDF、LVGL、ESP-SR、Opus 等依赖按各自许可证使用；外部曼波/GPT-SoVITS 引擎、音色权重及参考音频也按各自授权使用，根目录 MIT 许可不替代这些许可或服务商 API 使用条款。
