# D2 断开扬声器仍误插话与配对超龄修正

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

后续复测 epoch `3013152528` 在扬声器接线断开、无人讲话时再次误确认；本页的 FD／去直流／配对重试修正不能记为解决。已新增独立真实 DSP 隔离诊断，见[信号隔离记录](p43d2_signal_isolation_diagnostics.md)；本页历史证据保持不变。

更新：2026-11-11。设备附件 `6e99ce85-6c36-4e61-a319-2c2a236ad839`，服务端与另贴故障片段 epoch `2194261324`。用户明确断开扬声器接线、无人讲话。它比前一轮“用手堵住扬声器”更能排除扬声器声学回声；不能再将本轮直接归为房间回声或网络波动。

## 新证据与前批结论修正

启动实际 pipeline 为 `AEC(FD_LOW_COST, NLP_VERY_AGGRESSIVE)`，配置 type 3、AEC mode 5、NLP 2，播放期 VAD 没有关闭，输出也没有选择参考声道。前批 FD 模式修正确实运行，仍未解决误插话。本轮启动 ELF 为 `7ce40901a...`，与前批交付前缀不同，但完整 profile／pipeline 和新诊断证明不能简单解释为刷了旧 SR 固件。

| 插话事件 | 最新配对输入 mic mean／AC RMS | 参考 AC RMS | 精确输出帧 AC RMS | 确认窗 |
| --- | --- | --- | --- | --- |
| candidate，`at_us=73627087` | 278／15 | 2124 | 56 | 1 帧，AC RMS 56 |
| confirmed，`at_us=73811416` | 292／13 | 1397 | 140 | 10 帧／200 ms，AC RMS **99**，门限 **80** |

原始麦克风 RMS 大部分来自直流偏置，不能把 RMS 278／292 当作近端在说话。AFE 的 VAD 连续标为 speech，观察器的 AC 门又被窗口 99 超过，因此发生真实误取消。输入快照与输出未样本对齐，仍不能按 140÷13 计算 AEC 增益，或据此断言参考直接串入。

turn 2 先正常识别 11 字，设备随后取消回复并开启 turn 3；服务端 turn 3 空最终，AC RMS 69、peak 892。设备取消 ACK 匹配，确认至 ACK 约 218 ms、至首上行约 723 ms；服务端取消接收、清理、ACK 写出约 0 ms，媒体与事件循环指标正常。本次误开轮为空不能归于用户声音太小。

附件早期 49.736 秒的 `aec_or_session_unavailable` 发生时没有播放候选／确认，随后 AEC 仍 healthy／ready。它与 159.727 秒另贴的 `pair_age` 是不同现场；前者符合自动握手／缓存到期后取消导致观察关闭的代码路径，但旧日志没有细分原因，保留为推断。新批日志会区分 `speech_cancel_pending`、会话变化与 `aec_unavailable`。

## 独立的配对积压故障

用户直接粘贴的 turn 4 故障快照（不在上述附件文本内）：

- head sample 2472960，DMA sample end 2476288，年龄 3328 samples／**208 ms**，超过原 **200 ms** 守卫。
- 采集配对队列 10／10；AFE 队列 6、峰值 8；累计 `frontend_waits=33`，说明发生过 AFE 入队背压。
- 最近查询的参考 320 样本完整，无 prefix／suffix／interior missing，timeline healthy、unmapped 0；不能把错误名统一解释成参考丢失。
- AFE feed／output 为 2470400／2469888，feed 最大墙钟 44.666 ms；参考服务间隔最近 10.133 ms、最大 33.035 ms。墙钟含调度，不能直接当作 DSP CPU 利用率。
- 麦克风采集仍进展，overflow／invalid 为 0。`pair_age` 主动 invalidate 前端后打印通用 `microphone_unavailable`，不表示物理麦克风已掉线。

代码发现：空播放队列等待时，只检查 `reference_history_pending_` 是否需要短周期重试；即使参考已完整、AFE 暂满，仍可能再次等待完整 20 ms。前端释放槽位后不能及时获得新的配对机会，额外消耗剩余年龄余量。此遗漏可修复，但日志尚不足证明它独自解释全部 208 ms 积压；FD 实际算力与并发调度仍待实机。

## 本批处理

1. D1／D2 的 AFE 麦克风输入增加连续一阶 DC blocker，`y[n] = x[n] - x[n-1] + (32604/32768)*y[n-1]`，16 kHz 下截止频率约 12.78 Hz，以 Q16 状态和 int64 中间值保留小数，反馈向零截断以消除整数限环。首样本初始化避免启动直流脉冲，跨 320 样本帧保持状态，生命周期启动才复位；不逐帧减均值，不在每次播放／取消时重置，不新增预录等待。处理发生在 feed worker 已成功出队之后，重试不会重复滤波；参考声道与样本数量／offset 保持原样。
2. 手动 raw 和 C-only／默认单麦基线不经过这个新处理。现有输入 mean／AC 仍表示滤波前的配对输入，启动日志明确 `prefilter_input_stats=1`；不要把它与处理后输出声称为精确对齐。
3. AFE 暂满时标记交接等待，后台在无播放帧时采用 1 tick（当前配置 1 ms）阻塞重试；成功／无队列／终止时清除等待，恢复正常 20 ms 空闲等待。保持非阻塞采集、顺序交接和原 200 ms／10 槽限制，不扩大队列或放宽超龄判定。
4. 观察不可用的日志区分 AEC 与会话／取消状态，防止将健康 AEC 因取消而关闭观察误记为硬件前端故障。该诊断修正不自动恢复已失效的 AEC。

ESP-SR 的 `hps16fft`／`dl_rfft_s16_hp_run` 中 `hp` 是 high precision，不是 high pass。锁定库的麦克风输入路径检查未发现现成的时域 DC blocker；增加去直流有实测偏置依据，但仍是输入处理修正，不能在硬件对照前写成误报唯一根因已经证实。

## 验证边界与设备复测

本批软件验证通过：

- 前端 mono／SR AEC／FD AEC 为 33／45／49 个场景，包含六组独立滤波性质：常量及复位、DC 平移不变、跨块连续、80／100／1000 Hz 幅度保留、正负阶跃衰减至零、PCM16 极值与饱和。FD 生命周期检查满队列重试只处理一次、参考不改、跨帧与长段输出逐样本顺序和 offset 连续；mono／SR 保留原样本恒等断言。
- AudioService 的 M 与 MR/AEC 两种生产路径回归通过：临时满队列的 1 tick 无锁等待、原始完整配对的 FIFO 交接、恢复后的 20 ms 空闲等待，以及持续背压在 3200 samples 仍允许、3201 samples 严格失效；失败后不继续 1 tick 空转。
- 控制器 81 个场景通过，覆盖健康 AEC 的取消等待被准确记录为 `speech_cancel_pending`，与 `aec_unavailable` 区分。

独立 D2 已通过 ESP-IDF 6.1 构建，应用 **2,770,432 bytes**，3 MiB 应用分区余量 **375,296 bytes**，ELF SHA256 前缀 **`f5f7d7f03b`**。已核对真实编译数据库、对象时间、`pcm_dc_blocker.hpp` 在前端对象的依赖、应用内嵌 ELF 哈希以及模型未变。测试、源码与构建记录见本批验证报告（历史生成产物：`build/p43d2_dc_pair_verification.json`，当前目录未保留），前批报告不覆盖。只重建 D2；默认／C／D1 二进制仍为历史版本。本次未烧录、未打开串口、未修改服务端。

Host 替身验证滤波数值、状态／生命周期、重试时序与安全守卫，不能代替真实 ESP-SR AEC／VAD 声学效果。输入去直流与额外重试等待的修正已经实现，实际误插话是否消除、FD 持续吞吐是否满足预算，仍待本批设备复测。

附件首行有实际 `Brownout detector was triggered`，说明这次日志开头存在独立供电复位。它不能直接证明后续运行中的误插话／208 ms 超龄由供电引起，也不应隐藏或关闭 brownout 检测。即使扬声器线断开，功放与其他外设仍可能通电；复测使用稳定 USB 供电与可靠线材，并记录是否再次出现 BOD。不要同时接 USB 与未经确认电源路径的外部 5V。

关闭旧串口监视器，在 ESP-IDF 终端逐行执行：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build/barge-in-d2 -p COM7 -b 115200 flash monitor
```

先核对启动 ELF 前缀 `f5f7d7f03b`，并确认 `mic_dc_blocker=on coefficient=32604/32768 state=Q16` 和 SDK 的 `AEC(FD_LOW_COST, NLP_VERY_AGGRESSIVE)`。输入诊断继续标记 `prefilter_input_stats=1`，因此原始 mean 不会因滤波而自动变零；不要据此误判新处理没有启用。

连接就绪后按 `i`，先保持扬声器接线断开，提问触发回复，然后在界面显示播报期间保持无人讲话，检查是否误停播；下一轮由真人在界面显示播报期间讲完整短句；最后接回扬声器测试正常播报与真人双讲。分别记录条件，避免把断线基线与接线后的回声验收混成同一结果。检查是否还有误停播、空新轮、`pair_age`／本地检测失效及 BOD；保留第一次 candidate／confirmed 和失败快照即可。

关联：[FD 模式修正](p43d2_full_duplex_afe_fix.md)、[历史 AC 门](p43d2_false_barge_in_fix.md)、[D2 交接](p43d2_barge_in_handoff.md)、[实施进度](../realtime_voice_progress_20261112.md)。
