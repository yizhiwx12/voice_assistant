# 2026-11-12：自动监听的短促噪声起轮保护

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

## 反馈与代码依据

用户确认 D2 修正后扬声器恢复供电也能正常，基本效果认可；本轮另反馈正常自动识别对短促大动静敏感，进入识别后返回空结果。本轮没有新设备日志，因此不补记声源、精确误触发率或 SDK 实际保持时长。

代码确认：`AutomaticUtterance` 原先在 300 ms 静音就绪后，遇到第一个 `speech=true` 的 20 ms AFE 输出就进入 `Opening`；控制器随后发送 `speech.start`。它没有 D2 的能量持续性条件。因此，即使 VAD 人声标记只是一次瞬态或其后的保持，也会建立云端回合。本轮把过滤放在起轮前，不把真正的空 ASR 结果隐藏成其他状态。

## 实现与参数

- 新增独立 `VOICE_AUTO_START_CONFIRM_MS=160` 与 `VOICE_AUTO_START_MIN_AC_RMS=80`，同步根 `sdkconfig` 和 `sdkconfig.defaults`。仅用于空闲自动起音，不借用 D2 参数。
- 默认连续 8 个 20 ms VAD 人声帧，整窗 AC RMS 达到 80，至少 4 帧能量达标，且最后 3 帧至少 2 帧达标，才返回 `Triggered` 并允许开云端识别回合。统计来自正在确认的精确 PCM 帧，逐帧去均值只用于统计，不改波形、VAD 标记或上传内容。
- 确认前完整保留候选之前的 400 ms 预录，再追加 160 ms 确认音频。成功后按原 20 ms 节拍上传；起音确认不消耗确认之后的 2 秒握手预算。
- VAD 中断或能量不足时就地拒绝，缓存裁剪回最新预录长度；重新等待 300 ms 连续静音，避免同一次 VAD 保持重复起轮。拒绝的第一个静音帧计入重新就绪窗口。
- 已确认 D2 的 `adoptConfirmed` 直接交接，不再等待第二次起音确认。手动录音、700 ms 尾部静音、30 秒上限、AEC、麦克风增益、播放调度与 QIO 没有调整。
- 自动缓存预留“预录＋握手＋较大的自动／D2 确认窗＋4 帧”。当前 D2 根配置仍为 134 帧、87,904 bytes，因其 200 ms 确认窗大于本次 160 ms，没有增加当前 D2 的 PCM 缓存。非 D2 自动构建会相应预留 8 帧。

这是一项持续性过滤，不是完整的语音／噪声分类器。持续且有能量的噪声仍可能通过；不足 160 ms 的 VAD 人声或过弱起音可能被拒绝。某次窗口拒绝后，必须静音重新就绪，不能在同一段较晚变大的声音中直接重启；窗口内的弱起始音素和少量低能量帧仍可通过并完整保留。

## 诊断与软件验证

启动打印 `Automatic onset: confirm=160 ms min_ac_rms=80 ... preroll=400 ms`。显式 `?` 的 `auto_start` 提供 candidates、confirmations、rejected（VAD 中断）、energy_rejections（能量／持续性拒绝）、pending、window_frames、energy_active_frames、energy_tail_active_frames、frame_ac_rms、window_ac_rms 和候选／确认样本边界。计数以控制器配置以来为范围，正常轮次切换保留，统计窗口保留最近候选；D2 交接不计入空闲自动确认数。AFE 回调不打印逐帧日志，也不执行网络调用。

已通过以下主机检查（均不连接设备或云端）：

- `python tools/check_automatic_utterance.py`：30 场景，包括已有 20 个采样边界场景及 10 个新起音场景。新增覆盖瞬态＋VAD 长保持、直流、弱起音／间歇弱帧、强前段弱尾段、300 ms 重新就绪、400 ms 预录＋确认完整性、50 次拒绝缓存有界、重置／断帧、79／80 边界、PCM16 极值与已确认 D2 绕过。
- `python tools/check_conversation_lifecycle.py`：生产控制器回归，包括拒绝时无 `speech.start`／无上行、确认时刻才开始握手预算、一次起轮、逐帧原样上传预录和确认音频、D2 不重复起音确认。
- `python tools/check_barge_in_observer.py`：33 场景。
- `python tools/check_ui_text.py`：132 项。
- `python tools/check_local_frontend.py`：mono／SR／FD 34／46／50 场景及去直流信号检查。
- `python tools/check_audio_service_reference.py`：普通、AEC 与诊断钩子变体。
- 生产控制器另以 D1 自动、无插话半双工自动、仅手动三种主机配置执行 `-fsyntax-only -Wall -Wextra -Werror` 通过，覆盖新增配置及缓存表达式的条件分支；不等于 Xtensa／ESP-IDF 目标构建。

未执行固件构建、烧录、串口监视或服务启动；目标编译、实机灵敏度与性能仍待用户结果。用户确认的旧 D2 反馈不作为本次自动起音实机通过结论。

## 用户操作

继续在 ESP-IDF 终端使用根 `sdkconfig` 和 `build\barge-in-test`：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build\barge-in-test reconfigure
idf.py -B build\barge-in-test build
idf.py -B build\barge-in-test -p COM7 -b 115200 flash monitor
```

语音服务无代码变化，若当前已运行可继续使用；需要启动时另开终端：

```powershell
cd C:\esp-project\voice_assistant
.\.venv-voice\Scripts\python.exe server\voice_server.py --mode realtime --profile realtime_default
```

连接就绪按 `a` 测正常自动监听，或按 `i` 同时保留 D2。先安静片刻，制造之前会误起轮的短促动静；预期仍停留自动监听，没有云端识别空结果。随后用平时音量说一句完整短句，预期能识别、首字完整。只需复现这两种输入后按一次 `?`，如仍异常，提供 `auto_start` 和对应识别日志；无需重做 D1 或更换旧构建目录。
