# P3 设备实时链路操作与验收

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

2026-11-10：用户反馈主链路已能正常运行，但仍有偶发上行积压和待机服务错误。本轮加入短暂突发的有界等待、默认长期待机、正常空闲关闭、跨会话统计重置及 Wi-Fi 延迟对比配置。重启服务端及烧录新固件后仍需实机复验，P3 完整验收尚未通过。完整记录见 [连续录音断连问题与处理方案](p3_recording_disconnect_issues.md)。P3 使用手动发言边界；ESP-SR、VAD/AEC 和自动插话在 P4 接入。服务器继续保留独立选择 ASR、LLM、TTS 的配置。

## 1. 硬件与接线

目标为 ESP32-S3 N16R8，ESP-IDF 6.1。下表来自本项目现有驱动配置，未替代载板原理图或实物接线确认。

| 模块 | 接口 | GPIO | 正常测试现象 |
| --- | --- | --- | --- |
| ICS-43434 | I2S1 BCLK / WS / DIN | 4 / 5 / 6 | `?` 中说话时 RMS/peak 变化，录音样本数递增 |
| ICS-43434 | 声道选择 | 驱动读取左声道 | 模块 L/R 选择需匹配，否则采集可能近乎静音 |
| MAX98357A | I2S0 DOUT / BCLK / WS / SD | 15 / 16 / 17 / 8 | 启动音正常；回复按正常语速播放，`c` 停播 |
| 手动操作 | UART0 | 使用现有串口连接，115200 baud | 输入 `?` 得到 metrics 日志 |

GPIO7 当前没有启用按键。载板两个 USB-C 端口的路由尚未确认；当前控制输入来自 UART0，USB Serial/JTAG 作为 secondary console 时可看到日志却不接收这些控制字符。请使用连接 UART0 的串口，不能仅据“有日志”判断输入端口正确。

## 2. 启动服务与配置地址

在项目根目录 `C:\esp-project\voice_assistant` 执行：

```powershell
.\.venv-voice\Scripts\python.exe server/voice_server.py --mode realtime --profile realtime_default --check-config
.\.venv-voice\Scripts\python.exe server/voice_server.py --mode realtime --profile realtime_default
```

`--check-config` 不调用云；设备提交语音后将调用所选 ASR/LLM/TTS。密钥仍在 `server/config/server_secrets.json`，模型与音色在 `server/config/voice_config.json`；不要将云密钥填入固件。

在 ESP-IDF 的 `menuconfig → Voice Assistant` 核对：

| 配置项 | 用法 |
| --- | --- |
| `VOICE_WIFI_SSID` / `VOICE_WIFI_PASSWORD` | 填实际 2.4 GHz Wi-Fi |
| `VOICE_WIFI_POWER_SAVE` | 默认关闭以降低实时延迟；开启后使用 MIN_MODEM，供同路由器/同负载对比 |
| `VOICE_UPLINK_SEND_TIMEOUT_MS` | 默认 200 ms，范围 100–500；保留默认先测，不会重试失败音频包 |
| `VOICE_REALTIME_ENABLED` | 启用实时设备客户端，默认开启 |
| `VOICE_WS_SERVER_URL` | 例如 `ws://192.168.1.10:8000/ws/voice`，IP 替换为服务端电脑局域网 IP |
| `VOICE_AUDIO_SERVER_URL` | WS 地址留空时，从此 HTTP base URL 推导 `ws/wss` 并追加 `/ws/voice` |
| `VOICE_SERVER_PROFILE` | 默认 `realtime_default`，服务端 profile 必须存在 |
| `VOICE_TRANSPORT_CODEC` | 默认 raw Opus，调试时显式切为 PCM16；修改后重新构建和烧录 |
| `VOICE_MIC_GAIN` | 默认 1，范围 1–16；先看 RMS/peak/clipped 再调整 |
| `VOICE_PLAYBACK_VOLUME` | 默认 40%，范围 0–100 |

设备不能用 `127.0.0.1` 访问电脑。电脑和设备需要网络互通，服务端监听地址和防火墙应允许局域网端口 8000。保持当前 N16R8 的 PSRAM 配置：Opus 收发任务使用外部 RAM 栈，需启用 `SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`；主机检查不能验证设备 PSRAM 的运行状态。

用已配置的 ESP-IDF 终端构建，用户确认接线和串口后自行烧录：

```powershell
idf.py build
# COM7 是目前 VS Code 配置的端口；实际端口不同时应替换
idf.py -p COM7 -b 115200 flash monitor
```

上述命令供用户操作；此前软件实施阶段助手未执行烧录。用户后续已运行实机，实际烧录产物与下文软件构建哈希是否一致尚未核对。完整烧录使用项目生成的 bootloader、分区表和应用清单；P3 没有 ESP-SR model 镜像。

## 3. 手动对话

| 输入 | 行为和操作时机 |
| --- | --- |
| `r` | 服务连接就绪后准备一轮录音；等待屏幕显示“正在录音”后再说话 |
| `s` | 结束当前录音并提交识别；准备阶段按下时会提示等待，需进入录音后再按一次 |
| `c` | ASR 最终结果返回后，在等待回复、生成或播放期间取消；录音/提交期间忽略，等待提交完成后再用 `c` |
| `n` | 停播、丢弃旧会话并重新连接；不重发上一次问题 |
| `?` | 打印当前 heap、internal RAM、PSRAM、capture RMS/peak/clipped、播放队列，以及上行编码/发送耗时、队列峰值和上行栈余量 |
| `[` / `]` | 对话文字上一页/下一页；手动操作后暂停自动翻页 15 秒 |

输入小写字符即可，无需回车；终端若按行发送，输入字符后回车也可。一次发言最多约 30 秒，会自动提交。当前没有预录缓冲：准备识别期间采到的音频不上传，因此必须等待录音状态。空闲时也不持续上传静音；业务配置 `transport.idle_timeout_seconds=0`（默认）保持待机连接。若设为正整数，正常空闲到期后显示“待机会话已结束”，按 `r` 重建会话并开始全新录音；真实故障仍按 `n` 重连。

正文每页三行，每 5 秒自动翻页，底部显示页码；新一轮回答从第一页开始，流式增量不反复跳回第一页。设备最多保留 8191 字节有效 UTF-8（约 2700 个常用汉字，实际取决于文本），超过时页码旁出现 `...`；这是显示保留上限，不会截断服务端独立的 TTS 播报。

默认上行 16 kHz / 24 kbps、下行 24 kHz / 32 kbps，20 ms 单声道 raw Opus；PCM 调试模式采样率相同。服务器先解码 PCM 给 ASR，再把 TTS PCM 编码为 Opus，下行设备解码播放。

`?` 的 RAM 数值是当前空闲字节；`clipped` 是启动以来累计削顶样本数；`playback_frames` 最大 50，约 1 秒队列。`uplink` 日志包含本轮编码、网络发送和整帧处理的平均/最大耗时及数字返回码；整帧平均预算约 20000 us。新连接/新回合清零录音统计，`active_epoch/active_turn` 是当前会话身份，`metrics_current=1` 表示录音快照匹配当前身份。`queue/queue_peak` 包含控制项，音频容量约 500 ms；当前物理深度可能暂含尚待工作任务丢弃的旧 epoch 项。`stack_free_min` 是上行任务记录到的栈最低剩余字节，`heap_min` 是启动以来最小空闲堆，仍不代表 CPU 或所有任务资源已验收。

周期汇总每 100 帧约 2 秒一次，由控制任务打印。异常时结合 `conversation: first_failure`、`voice_transport: first_failure` 与服务端 `voice_failure first=1` 的时间、epoch/turn/stream 查看首因，后续清理日志不会覆盖首个屏幕提示。首个控制器指标快照可能早于正在发送的帧完成，可结合传输层日志和后续 `?`。字段解释见 [问题记录](p3_recording_disconnect_issues.md)。取消日志的 `Local playback fence and DMA reset` 是本地软件屏障耗时，实际停播延迟仍需实测。

## 4. 验收记录

以下按用户反馈更新；尚未覆盖的项目继续标为待测，不因构建或主机测试通过自动勾选：

| 场景 | 通过条件 | 当前记录 |
| --- | --- | --- |
| 上电/串口/连接 | 启动音、屏幕和 Wi-Fi 正常，`?` 有响应，进入服务就绪 | 待测 |
| 单轮 `r → 说话 → s` | ASR、回答、扬声器声音正确；首字和尾音完整，语速正确 | 用户反馈短录音能识别播报；首尾音/语速尚未逐项确认 |
| 持续录音 | 2、5、10、20 秒及接近 30 秒的发言能上传并正常提交，队列无持续增长；30 秒上限正确自动提交 | 风扇背景声失败已确认是 ASR receive_timeout；已改为音频发送进度保护及独立最终结果预算，183 项离线测试通过。须重启服务，分别复测持续说话、背景声自动结束、手动提交及空识别后下一轮 |
| 扬声器重复初始化 | 多轮、取消和重连时使能正常，无重复占用告警 | 已分离 SD 与 DMA 生命周期，100 次静音重建主机回归通过；待设备实测 |
| 连续三轮 | 回合不串，文本与声音对应，下一轮可正常开始 | 待测 |
| 播放中 `c`，再开启新轮 | 及时停播，旧包/旧 DMA 音频不重新出现，新轮正常 | 待测 |
| 回复中 `r` | 先停旧回复，等待录音状态后新问题可提交 | 待测 |
| 准备/录音中 `c/s` | 提示与上表一致，不因迟到启动任务误开录音 | 待测 |
| Wi-Fi/服务断开后 `n` | 新会话恢复，旧语音不复活，不重发旧问题 | 待测 |
| PCM 调试回退 | 新会话按 PCM 格式互通，无采样率或包长错误 | 待测 |
| 上电 LCD 回调 | 不再出现 `on_color_trans_done ... overwritten`，刷新和启动文字正常 | 已统一回调所有权，待烧录复验 |
| 长时间默认待机 | 默认 0 时静置至少 5 分钟，心跳正常、无服务错误，随后 `r` 正常录音 | 用户反馈新版长时待机未再报错；具体时长未记录，至少 5 分钟及随后录音仍待确认 |
| 配置有限待机 | 临时配置正数秒数并重启，空闲正常 1000 关闭；屏幕显示待机结束，`r` 建立新 epoch 且只启动新录音 | 本地 WebSocket/固件主机回归覆盖，待实测；验收后恢复 0 |
| 长回答文字 | 超过三行和 319 字节仍可翻页，自动翻页及 `[` / `]` 正常，中文无半字，声音完整 | 67 项状态/UTF-8 主机检查通过，LCD 视觉和听感待测 |
| 网络短暂停顿 | 检查 queue、decode/feed/send、loop lag 和 receive burst；恢复或明确失败后可 `n` 重连，无旧流串入 | 待测，不以无限扩队列/丢 Opus 包掩盖停顿 |
| Wi-Fi 延迟对比 | 同一接入点、同音频负载分别关闭/开启省电，记录 burst/相对延迟/send_max，必要时与手机热点对比 | 默认关闭省电；不能据设置推断网络根因已确认 |
| 长轮/资源/电源 | 无复位、看门狗、栈溢出、持续内存下降或无法恢复的队列积压 | 待测 |

建议保存启动、`?`、取消、重连和异常日志到 `server/data/` 或 `build/`，隐去 Wi-Fi 与个人信息。记录距麦克风距离、音量、问题文本、首尾截断、停播听感、内存/栈最低水位和复位原因。P3 未完成验收继续跟踪；按用户决定先开展 P4.1 基础接入，随后按各步验收推进，见 [P4 本地语音前端清单](../p4/p4_local_audio_checklist.md)。

## 5. 最新软件验证与固件

- 2026-11-10 ASR 无响应录音修复：完整服务端 **183 项通过**，无跳过（31.399 秒）；针对性 **40 项通过**，实时配置检查通过。日志 `build/p3_silent_recording_fix_unittest.log`、`build/p3_silent_recording_targeted_tests.log`。只改服务端，固件沿用下方已有产物，重启服务后实测；详见 [问题记录第 12 节](p3_recording_disconnect_issues.md#12-2026-11-10确认响应等待超时并修复)。
- 2026-11-10 后续 ASR 原因诊断：完整服务端 **177 项测试通过**，无跳过（28.655 秒），日志 `build/p3_asr_failure_diagnostics_unittest.log`；配置检查通过。只增加服务端固定原因/数字码诊断，规范云响应和超时策略保持原行为，异常状态码收紧为整数；不需新固件。用户反馈新版长时待机未再报错，无人声风扇录音约 30 秒的 ASR 失败仍待具体原因字段确认，见 [问题记录第 11 节](p3_recording_disconnect_issues.md#11-2026-11-10新版服务下的无语音长录音失败)。
- 2026-11-10 完整离线单元测试 **166 项通过**，无跳过（22.634 秒）；新增大于队列容量的顺序突发、持续慢消费、相对媒体延迟、取消与 worker 失败、正常待机关闭及资源释放回归。日志：`build/p3_stability_fixes_unittest.log`。
- `python tools/check_voice_transport.py`：**7 组**生产 VoiceClient 场景通过，包括关闭帧先于应用消息分发时保持 FIFO，以及正常 1000 关闭不抢先记录传输故障。日志：`build/p3_stability_transport_checks.log`。
- `python tools/check_conversation_lifecycle.py`：**7 组**生产会话控制器场景通过，包括统计重置、旧 End 尾部污染新轮统计的竞态、受控错误分类和待机结束后按 r 重握手。日志：`build/p3_stability_controller_checks.log`。
- 文字显示本轮未改动；此前 `python tools/check_ui_text.py` 的 **67 项**状态/UTF-8 检查通过，LCD 视觉验收仍待设备确认。
- ESP-IDF 6.1 / ESP32-S3 最终构建通过；编译数据库和对象时间确认修改源码已编译。新增 Kconfig 已重新配置，实际省电关闭、发送预算 200 ms。日志：`build/p3_stability_reconfigure.log`、`build/p3_stability_fixes_build.log`、`build/p3_stability_final_build.log`。
- 新固件 `build/voice_assistant.bin` 大小 **0x1fb340 / 2,077,504 字节**，3 MB 应用分区剩余约 34%；SHA-256：`0ed60f72fbba0c96bad12630623d2c86fada19607422bbc96de55561a24ba034`。运行时 internal/heap/PSRAM 水位仍需用 `?` 实测。
- 本地实时配置及原生 Opus 加载检查通过；仅新增本地业务配置 `idle_timeout_seconds=0`，凭证文件未修改，没有调用云 API。

本轮未执行烧录。请重启原服务端进程并按既有方式烧录新固件，然后复测 5/10/20/30 秒录音、连续三轮、网络停顿、至少 5 分钟默认待机、长回答、打断及服务端重启后重连。可选有限待机的边界：如果设备已经进入“准备录音”，恰好才收到服务端先前决定的空闲关闭，会保守报 `idle_close_active`，按 `n` 重连；不会重放旧音频，默认 0 不触发该策略。积压故障必须保存同一 epoch 的 `voice_failure first=1` 和设备日志，才能据新指标确定实际卡点；不能仅凭构建通过把断连标为已彻底消失。

## 6. 第一轮软件验证（历史）

- ESP-IDF 6.1 / ESP32-S3 修复构建通过，四个修改的生产 `.cpp` 已核对编译数据库及对象时间；固件为 `0x1fa540` 字节（2,073,920），3 MB 应用分区剩余约 34%。构建日志在 `build/p3_disconnect_fix_build.log`。
- 服务端离线单元测试 137 项全部通过，无跳过；实时配置检查通过，未调用云 API。日志：`build/p3_disconnect_fix_unittest.log`。
- `tools/check_firmware_protocol.py`：真实 C++ 协议实现与 Python 交叉检查，21 组向量通过。
- `tools/check_firmware_opus.py`：锁定 ESP-Opus 源码与真实固件 wrapper 在主机构建，与服务端 libopus 双向互解，14 组长度/采样率场景通过。
- `tools/check_audio_driver.py`：真实 MAX98357A 驱动的 100 次静音 DMA 重建、14 种注入失败恢复、所有权交接和无 SD 配置通过。
- `tools/check_voice_transport.py`：真实 `VoiceClient` 的 5 组首因/关闭码/重连/本地停止/部分发送/锁超时与日志脱敏场景通过。
- ASR 连续 30 秒离线输入：确定性调度抖动模拟队列峰值 9/25；本机 Windows 实时钟 mock 发送回归峰值 2/25，完整样本数均为 480000。测试过程不连接云服务。

本次修复 `build/voice_assistant.bin` 的 SHA-256 为 `818e14144832ad760161b23f9373f9079b7942a0c10ab3bc1381b9cc66cede8d`。重新构建后哈希可能改变，验收时以实际烧录产物为准。此前 `p3_final_build.log` 对应旧固件，不能作为本次修复的烧录标识。

上述交叉测试验证协议与编解码逻辑，不验证 Xtensa 性能、声学质量、I2S、DMA、电源或用户实际听到的内容。
