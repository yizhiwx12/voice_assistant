# 2026-11-12：首答绑定失败与小智音频实现对照

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

本次依据本地 `C:/esp-project/xiaozhi-esp32-2.5.2` 源码，以及附件 `955cbedc-1084-4b8b-b80e-9488cdc9ed6c`、`5488418a-50e9-479d-8be2-fe8c8b61043b`。小智目录是用户本地副本，本次未联网核验其是否为上游原版或最新版本，未修改小智源码。

## 新故障的确定证据

两份固件 ELF 前缀均为 `3b6a07e28`，启动配置为 `barge_in / FD_LOW_COST / NLP_VERY_AGGRESSIVE`，参考任务在 CPU0。

| 证据 | 附件 955 | 附件 548 |
| --- | --- | --- |
| 首答前 `playback_serial / submitted_frames / content` | `0 / 0 / 0` | `0 / 0 / 0` |
| 首答前参考丢块 / discontinuity | `9 / 1`，第191行 | `10 / 1`，第192行 |
| `playback_binding` | 32.387秒，第213行 | 30.040秒，第214行 |
| 冻结 AFE 队列 / frontend_waits | `0 / 0` | `0 / 0` |
| 冻结 feed/output samples | `434176 / 434176` | `396800 / 396800` |
| 当时队首年龄 | 84ms | 92ms |
| 会话结束时收到音频二进制消息 | `0` | `0` |

故障链为：初始静音期间参考 ISR 队列溢出 → 历史 `discontinuity` 锁存 → 首次 `bindPlaybackSerial()` 拒绝 → AEC 关闭 → 插话观察 `aec_unavailable` → 控制器误归类为 `downlink_format`。

本次不是先前 `pair_age=204ms` 的复现。故障时麦克风 overflow 为0、最近参考查询完整，无法用 AFE 队列满来解释这次首错。日志没有冻结首次 TX 丢块的时间和所在任务，不能据此证明它具体发生在哪条启动日志、哪次抢占；启动时先开启 TX 再创建消费者、以及开启后的 UART 输出，确实存在消耗8块 ISR 队列预算的窗口。

## 小智的实际参数

以下任务参数指开启 `CONFIG_USE_AUDIO_PROCESSOR` 的应用层路径；未显式设置的 ESP-SR 库内线程参数不能由此推断。

| 项目 | 本地小智实现 | 本项目当前实现 |
| --- | --- | --- |
| 输入 | CPU0，优先级8，栈6144B；每次160样本/10ms，直接交给 AFE 适配器 | CPU1，优先级7，栈6144B；320样本/20ms，先做参考配对 |
| AFE feed | 输入任务累计至 AFE chunk 后直接 feed | 独立 CPU1/优先级6/栈8192B，应用队列12×20ms |
| AFE fetch | 不固定核，优先级3，栈4096B；`portMAX_DELAY` | CPU0，优先级6，栈8192B；50ms超时 |
| 播放 | 不固定核，优先级4，栈4096B；独立阻塞写 codec | CPU0，优先级6，栈6144B；与参考转换、配对共用任务 |
| Opus | 不固定核，优先级2，栈24576B；默认60ms帧 | 上行独立任务优先级5，栈49152B；下行解码在网络分发路径，20ms帧 |
| 编码/播放队列 | 各2×60ms | 播放50×20ms；AEC 配对10×20ms，超龄200ms失败 |
| 网络缓冲 | 解码正常入队门槛20包/1.2s；发送40包/2.4s | 不是同一协议和交接路径，不能直接替换参数 |
| AEC | FD / LOW_COST / VERYAGGR | FD / LOW_COST / VERYAGGR，已经相同 |
| VAD/NS/AGC | VAD mode0，min noise100ms；NS/AGC关闭 | VAD mode0，speech/noise/delay各128ms；NS/AGC关闭 |
| ESP-SR | 2.4.7 | 2.6.1 |

小智解码队列的物理 `FixedQueue` 容量为 `10000/60=166` 包，供录音测试交换；正常解码入队仍用20包门槛。不能把166包当作普通播放缓冲建议。

源码位置：

- [任务创建](C:/esp-project/xiaozhi-esp32-2.5.2/main/audio/audio_service.cc:125)、[10ms输入](C:/esp-project/xiaozhi-esp32-2.5.2/main/audio/audio_service.cc:311)、[锁外硬件写](C:/esp-project/xiaozhi-esp32-2.5.2/main/audio/audio_service.cc:351)。
- [AFE参数](C:/esp-project/xiaozhi-esp32-2.5.2/main/audio/engines/afe_audio_engine.cc:144)、[fetch任务](C:/esp-project/xiaozhi-esp32-2.5.2/main/audio/engines/afe_audio_engine.cc:222)、[阻塞fetch](C:/esp-project/xiaozhi-esp32-2.5.2/main/audio/engines/afe_audio_engine.cc:419)。
- [队列参数](C:/esp-project/xiaozhi-esp32-2.5.2/main/audio/audio_service.h:40)、[编码满时丢旧输出帧](C:/esp-project/xiaozhi-esp32-2.5.2/main/audio/audio_service.cc:584)、[日志移出锁并每秒限流](C:/esp-project/xiaozhi-esp32-2.5.2/main/audio/audio_service.cc:597)。

小智的关键原则是：阻塞播放独立运行，网络拥塞不能反向阻塞 AFE fetch。它丢弃的是 AFE **之后**的编码/发送队列旧帧，不能照搬到 AEC 之前的 mic/reference 配对队列。

## 参考信号的硬件区别

小智的硬件回采板（例如立创板）使用 codec 的麦克风与播放参考输入，同采样率、共享 I2S 时钟；完整多声道一起转换到16k后送入 MR AFE。见 [同率双工](C:/esp-project/xiaozhi-esp32-2.5.2/main/audio/codecs/box_audio_codec.cc:99)、[输入参考通道](C:/esp-project/xiaozhi-esp32-2.5.2/main/audio/codecs/box_audio_codec.cc:202)、[板级参考配置](C:/esp-project/xiaozhi-esp32-2.5.2/main/boards/lckfb/szpi-esp32s3/config.h:6)。

小智也有软件参考，例如 [Box Lite](C:/esp-project/xiaozhi-esp32-2.5.2/main/boards/espressif/esp32-s3-box-lite/box_audio_codec_lite.cc:232)：1920样本缓存，写 codec 后缓存播放 PCM，读麦克风时取参考，空时补零。这些例子仍使用同率、共享双工时钟；普通 NoAudioCodec 默认不提供参考。不能据此认为所有小智板型都具备设备 AEC。

本项目 ICS-43434 RX16k 与 MAX98357A TX24k 使用独立 I2S，需要 DMA 完成时间映射、24→16k重采样、逐样本有效性检查。小智的优先级数字和软件缓存不能直接解决这条路径的时钟对齐。本轮不改变引脚、时钟、AEC档位或任务优先级。

## 本轮修复和边界

1. 历史 `discontinuity` 与禁止绑定的 `binding_fault` 分离。只有首次提交前、serial为0、功放稳定关闭时发生的 ISR 队列溢出不锁存绑定故障；仍保留丢块计数、gap、缺失样本坐标。部分写入、非法 DMA、声道不匹配、切换期或播放期丢块仍禁止绑定。没有把未知参考标记成有效静音。
2. TX配置日志移到 `i2s_channel_enable()` 之前，消除该条日志在启用 ISR 后等待 UART 锁的风险。此修改不能证明以后不会因其他调度因素丢块。
3. 格式字段校验与 `beginPlayback()` 错误分开。格式错误保留 `downlink_format`；本地初始化失败记录 `playback_begin` 和原始返回码。
4. 新增初始静音溢出后首答绑定、缺口仍可见，以及有声/已提交/已绑定/功放切换/非法DMA/部分写入仍拒绝绑定的主机回归；增加控制器错误归类回归。

`binding_fault=0` 仅说明允许绑定，不代表声学消除已达标。参考缺口仍由有效性检查和200ms年龄保护处理；极早首答若参考未恢复仍可能失败。之前的 `pair_age` 问题和声学双讲质量仍需设备验证，不能由主机测试宣称解决。未来若仍有播放期配对停滞，应按小智的解耦原则检查播放写入与参考服务是否需要独立任务，而非继续扩大队列或放宽年龄保护。

主机检查：`check_playback_reference.py`、`check_audio_driver.py`、`check_audio_service_reference.py`、`check_conversation_lifecycle.py`。检查只验证软件逻辑、错误分类和模拟生命周期，不测真实双核抢占、I2S时序或声学效果。

用户在 ESP-IDF PowerShell 中构建并烧录，继续使用 `barge-in-test`，该目录现有 `project_description.json` 指向项目根目录的 `sdkconfig`：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build\barge-in-test build
idf.py -B build\barge-in-test -p COM7 -b 115200 flash monitor
```

服务端在另一个终端启动：

```powershell
cd C:\esp-project\voice_assistant
.\.venv-voice\Scripts\python.exe server\voice_server.py --mode realtime --profile realtime_default
```

设备验收先核对首答实际播放成功、无 `playback_binding`/`playback_begin`，`binding_fault=0`、`aec_pairing active=1 error=0`。初始静音历史丢块仍可能使 `discontinuity=1`，必须结合时段与 `binding_fault` 判断；真实播放期间继续增加丢块仍属故障。固件构建、烧录、服务启动和硬件测试均由用户执行，本轮未执行。
