# P2 默认组合云端小样本验收

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

日期：2026-11-09。结论：默认组合的 PC → 本地 WebSocket → 豆包 ASR → DeepSeek SSE → 火山 TTS → PC WAV 已通过；同连接三轮、首音频后取消及下一轮恢复、显式 PCM 回退均通过。可以进入 P3 设备录放与 WS 实施。

本次使用生成语音，不使用用户录音。PC 只保存 WAV，不播放、不发送 `playback.done`，本记录不代表 ESP32、真实麦克风、扬声器、VAD 或 AEC 已验收。LLM 回答中提到的功能也不代表本项目已实现对应能力。

## 配置与输入

服务运行在临时本机地址 `127.0.0.1:18080`，选择 `realtime_default`；测试结束后停止临时服务。业务配置和密钥文件未修改。

| 层 | 本次实际选择 |
| --- | --- |
| ASR | `doubao_2` / `bigmodel` / `volc.seedasr.sauc.duration` |
| LLM | `deepseek_default` / `deepseek-flash`，SSE，默认预算 1024 token |
| TTS | `volc_default` / `seed-tts-2.0` / `zh_female_vv_uranus_bigtts`（Vivi 2.0），PCM16 mono24k |
| 设备模拟传输 | raw Opus，上16k/下24k，20 ms；另测 `pcm_s16le` 显式回退 |

输入使用同一火山音色合成后转换为 PCM16 mono16k，文件位于忽略的 `server/data/`：

| 文件 | 合成文本 | 时长 |
| --- | --- | --- |
| `p2_question.wav` | 请用一句话告诉我，你能做什么。 | 约 2.374 秒 |
| `p2_cancel.wav` | 请详细介绍语音助手的工作流程。 | 约 2.765 秒 |
| `p2_recovery.wav` | 请只回答：测试成功。 | 约 1.977 秒 |

## 检查结果

| 场景 | 结果及证据 |
| --- | --- |
| Opus 单轮 | 非空 ASR、LLM 回答、正常结束的 24k WAV；`p2_opus_report.json`、`p2_opus_reply.wav` |
| 同输入 PCM | 相同有效输入 37991 样本；非空回答及正常结束 WAV；`p2_pcm_report.json`、`p2_pcm_reply.wav` |
| 同连接三轮 | 第一轮 complete → 第二轮首音频后 cancelled → 第三轮 complete；`p2_multiturn_report.json`、`p2_multiturn_turn01/02/03.wav` |
| 取消恢复 | 第二轮已收到一个音频包，再在本地封禁身份、关闭解码流并发取消；约 62 ms 收到 ACK；第三轮回答“测试成功！” |
| 资源释放修复后复验 | 再测同连接两轮：首音频取消 → 下一轮“测试成功！”；取消确认约 78 ms，恢复轮提交到首音频约 1500 ms；`p2_cleanup_cancel_report.json` |
| 旧数据隔离 | 云测试本次没有观测到取消后的迟到包；离线 WS 测试额外注入重复旧 ACK 和非法旧音频，确认它们不结束新轮或进入解码器 |

立即取消时第二轮 WAV 为零有效样本是预期结果：客户端已收到音频包，但保留的末帧尚未取得有效结束位置，取消后不再输出。不能据此声称发生过扬声器播放或测得设备停播时延。

Opus 转写为“请一句话告诉我，你能做什么？”，PCM 为“清一句话告诉我你能做什么？”。两者都没有完整保留输入中的“用”，且有首字同音字差异；只证明当前短句可识别，不能据一个样本推断压缩对准确率的影响。真实麦克风、弱音、背景噪声、长句仍需后续测试。

| PC 端计时，单位 ms | Opus 单轮 | PCM 单轮 |
| --- | ---: | ---: |
| 最后一帧/发言结束提交 → ASR final | 360 | 281 |
| ASR final → LLM 首个回答增量 | 1687 | 1610 |
| LLM 首个回答增量 → 首个非空音频包 | 422 | 390 |
| 发言结束提交 → 首个非空音频包 | 2469 | 2281 |

这只是一次基线，包含本地调度与网络波动，不是稳定性统计或性能保证；首音频包不等于首播，也没有测量云内部“首段文本提交 → TTS 首音频”的独立时延。三轮中的普通回复与恢复轮首音频分别约 2515 ms、1906 ms。

相同输入的上行音频：Opus 为 120 包、6617 字节 payload、10457 字节含应用头；PCM 为 119 包、76160 字节 payload、79968 字节含应用头。含头减少约 **86.9%**；Opus 多一个包用于前瞻排空。统计仅包含音频 payload 和每包 32 字节应用头，不含控制 JSON、WebSocket、TCP、TLS 或 IP 开销。两次 LLM 回答和生成音频长度不同，不以绝对下行字节数比较压缩率。

## 本次发现并修复的问题

- 云 ASR 首次握手实测约 562 ms，超过约 500 ms 的上行队列容量。PC 工具先等待当前轮 `speech.started`，再发送已有 WAV。P3 需在就绪后才显示可讲话；连续采集的预录必须有界。
- 旧 PC 工具只支持单轮。现在支持同连接多 WAV、递增 turn/stream、指定轮次及首音频后的取消，保存逐轮 WAV 和 JSON 时序/字节报告。
- 活动回复的一条取消请求曾产生两个 ACK；现改为每条匹配请求只确认一次，保留已结束回复上重复取消的幂等确认。
- 取消恰好发生在向 TTS 送文本时，LLM 异步生成器可能尚未显式关闭。现将其纳入受保护的清理流程，等待上游关闭后再确认取消；回归测试强持有生成器，确保不依赖垃圾回收。修复后另行完成真实两轮取消恢复复验。
- 实时 LLM 曾沿用非流式的 128 token 默认预算，真实 SSE 诊断确认出现 `finish_reason=length`，可导致正文为空或中途截断。流式默认改为 1024；非流式仍为 128，显式 `max_tokens` 优先。非正常结束不再被当作完整回答，不自动重试已经生成的回复。
- 验收工具现在要求 `reason=complete`、ASR/回答非空、有效音频非空和音频流正常结束；缺项、厂商错误或未发生预期取消都会非零退出。报告不保存密钥、端点或异常原文。

初次失败与修复前的报告保留为 `p2_initial_*_report.json`。所有运行期 WAV 和 JSON 均被 `.gitignore` 忽略，不进入源码目录。最终全套 **127 项离线测试通过，无跳过**，包括 8 种 fake 组合；这不代表 8 种真实云组合均已验证。Qwen LLM、CosyVoice 仍待实际配置，旧 Qwen/DeepSeek HTTP 只做了离线回归。

## 复现命令

从项目根目录执行；以下操作会调用所选云服务。先准备自己的 PCM16 mono16k WAV，或复用本次生成的输入文件。

```powershell
# 终端一：本机验收用临时端口
.\.venv-voice\Scripts\python.exe server/voice_server.py --mode realtime --profile realtime_default --host 127.0.0.1 --port 18080

# 终端二：单轮及显式 PCM 对照
.\.venv-voice\Scripts\python.exe tools/voice_client.py server/data/p2_question.wav --url ws://127.0.0.1:18080/ws/voice --output server/data/p2_opus_reply.wav --report server/data/p2_opus_report.json
.\.venv-voice\Scripts\python.exe tools/voice_client.py server/data/p2_question.wav --codec pcm_s16le --url ws://127.0.0.1:18080/ws/voice --output server/data/p2_pcm_reply.wav --report server/data/p2_pcm_report.json

# 同连接三轮：第二轮首音频后立即取消，第三轮继续
.\.venv-voice\Scripts\python.exe tools/voice_client.py server/data/p2_question.wav server/data/p2_cancel.wav server/data/p2_recovery.wav --url ws://127.0.0.1:18080/ws/voice --cancel-turn 2 --cancel-trigger first-audio --cancel-after-ms 0 --output server/data/p2_multiturn.wav --report server/data/p2_multiturn_report.json
```

下一阶段按 [实施方案](../realtime_voice_plan_20261112.md) 的 P3 接入 PCM16 转换、连续录放、双向 Opus、WS、独立任务、有界队列与手动打断。尚无实际固件录放延迟、DMA 清理和资源水位数据；ESP-SR/VADNet/AEC 在 P4 验证后再启用自动插话。分句最大等待时间与更细的服务内部延迟仍需完善，当前分句按句末标点、120 字上限或 LLM 结束提交。
