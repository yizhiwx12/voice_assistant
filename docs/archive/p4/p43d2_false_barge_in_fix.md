# D2 安静播报误打断：证据与候选窗口修复

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

后续实机反馈：epoch `4243929666` 无人讲话时确认窗 AC RMS 达到 1600，仍通过 80 门限并误打断。本页低 AC 门为历史修复，未解决该强交流误报；后续已发现 D1／D2 仍运行 `SR_LOW_COST, NLP_OFF`，本批配置修正及复测边界见[全双工 AFE 修正](p43d2_full_duplex_afe_fix.md)。

更新：2026-11-11。用户确认本次 turn 5、7、12 播报期间无人讲话，因此这三次属于误打断。上一批只加诊断，未解决误确认；本批增加候选窗口的 AC 能量条件，设备效果仍待验证。

## 本次日志结论

配对附件：设备 `ec93a42c-795f-416a-b279-4c6ff1d1c73e`，服务端 `3290ba58-31de-4dd2-a8d9-b1784f2d2432`。设备附件从 epoch `591496209` turn 5 中途开始，不能据此补认更早设备过程。

| 无人讲话的旧回复 | 确认时 mic RMS／peak | 确认时 AFE 输出 RMS／peak | 确认至取消 ACK | 新识别结果 |
| --- | --- | --- | --- | --- |
| turn 5 generation 3 | 422／472 | 425／529 | 224 ms，match=1 | turn 6 empty，PCM RMS 51 |
| turn 7 generation 4 | 407／440 | 420／710 | 216 ms，match=1 | turn 8 empty，PCM RMS 67 |
| turn 12 generation 7 | 396／670 | 282／503 | 213 ms，match=1 | turn 13 empty，PCM RMS 42 |

三次均达到连续 10 帧 VAD 人声条件后停播、取消并开启新识别。取消屏障已匹配成功，没有重现此前的取消确认超时；确认至首上行约 786／825／793 ms。这一轮支持旧 ACK 修复在这三次交接中有效，不等于长期或全部时序验收通过。

turn 6／8／13 是无人讲话却误开启的新轮，其低能量空识别是误触发后的结果，不能解释成用户讲话太小。服务端该 epoch 共 14 轮，8 轮有文字、6 轮为空；其余空轮缺少逐轮声源标记，仍单独保留。

去重后可见六组候选／确认：turn 5、7、10 两次、11、12。turn 10／11 处于只观察模式，没有实际取消；其中哪些对应主动讲话未标注，不把全部六次都认定为误触发。重复打印同一事件需按 `event + epoch/turn/generation + at_us + output_sample` 去重。

## 与网络、AEC 健康的关系

服务端较早 epoch `2658432150` turn 1 存在独立上行故障：接收空档 1484 ms、媒体落后 1419 ms。后续 epoch `591496209` 各轮最大接收空档为 109～203 ms、媒体落后为 0～193 ms、事件循环最大 25 ms，没有同类积压证据，不能用这一旧网络故障解释后续误确认。

设备快照为 `ready=1 aec_active=1 error=0`，采集 overflow、参考 gaps／errors／unknown、分配失败均为 0。日志中 AFE ringbuffer empty 警告需要继续观察供给节拍，但这些快照没有重现旧“本地前端失效”。参考时线健康也不能证明声学回声已充分消除，`calibrated=0` 仍保留。

多处麦克风 RMS 接近 peak，空闲时也有 394／491、353／473、293／391。它们提示直流偏置、低频占优或波形异常的可能性；旧日志没有均值与 AC RMS，输入／参考／输出也未样本对齐，因此不能据此断言 DC 已证实、AEC 已失效，或计算回声消除量。

## 本批实际修改

`BargeInObserver` 同时服务 D1 观察与 D2 打断。保留原连续 VAD 200 ms 确认窗口，在窗口中累计每个 20 ms、320 样本 AFE 输出帧去除自身均值后的能量，再计算整个窗口的 AC RMS。

- 只有连续 VAD 条件与窗口 AC RMS 都通过，才进入 confirmed；不把单个弱音帧当成静音，不因短暂弱音重新计算人声时长。
- `VOICE_BARGE_IN_MIN_AC_RMS` 为 PCM16 单位，默认 80、Kconfig 范围 1～2048。80 是未校准的工程初值，后续必须用安静播报与真人讲话对照评估漏检和误检。
- 低 AC 窗口增加 `energy_rejections` 并进入 cooldown；不停止播报、不发送取消、不创建云端轮。失败候选按原 cooldown 生命周期丢弃，连续 VAD 静音 300 ms 后才重新 armed，持续低能量误报不会反复确认。
- 去均值只用于判定统计，不滤波或改写原 PCM、样本 offset、VAD 标签。有效候选仍沿原 400 ms 预录及 D2 音频交接路径保留完整音频。
- 不改变空闲自动起轮、手动录音、AEC DSP、网络超时或上行媒体预算。具有足够 AC 能量的残余回声仍可能通过，本修复不是完整的回声／真人分类器。

## 新日志如何判断

`barge_in_energy` 增加同一个确切 320 样本输出帧的 `frame_sample`、`frame_mean`、`frame_rms`、`frame_ac_rms`、`frame_peak`、`frame_vad`，并标记 `frame_stats_aligned=1`。`window_ac_rms`／`window_frames` 是实际判定窗口，`min_ac_rms` 为本次门限；低能量拒绝记录 `event=rejected_low_ac`，观察快照带累计 `energy_rejections`。

这些确切输出帧指标与旧的异步 `mic_rms/ref_rms/out_rms` 分开解释。旧字段继续标记 `mic_ref_output_sample_aligned=0`，不用于证明输入与输出的瞬时能量比。比较 `frame_mean` 与 `frame_ac_rms` 可识别均值占优现象，但仍需受控声源场景判断来源。

## 软件验证与设备验收

本批主机测试通过：候选观察器 32 个场景（原 24＋8 个 AC 活动与 PCM 边界），控制器生命周期 80 个场景（原 78＋2 个实际不取消／完整交接回归），自动发言缓存 20 个场景。覆盖常值／近零误报拒绝、带直流偏置的有效交流通过、弱音帧不中断窗口、门限边界和满幅计算，以及有效候选的原 PCM／VAD／offset 完整保留。审查未发现新增分配、等待或 AFE 回调日志，统计工作固定有界。

独立 D2 已经 ESP-IDF 6.1 reconfigure 与构建通过，生成配置确认 `VOICE_BARGE_IN_MIN_AC_RMS=80`、`VOICE_BARGE_IN_CONFIRM_MS=200`，AEC／D1／D2 编译开关均开启。应用 2,768,432 bytes，3,145,728 bytes 应用分区剩余 377,296 bytes；ELF SHA256 前缀 `008d03b334`。编译数据库、对象时刻和应用内嵌 ELF 哈希已核对，AFE 实现／头文件与上一诊断批一致，模型哈希与此前交付一致。完整证据见本批验证报告（历史生成产物：`build/p43d2_acgate_verification.json`，当前目录未保留）。本轮只重建独立 D2，不将旧默认／AEC／D1 二进制写成已重建。

串口已枚举到 COM7，本次没有打开串口、复位或烧录设备，没有本批声学复测结果。关闭旧串口监视器后，在 ESP-IDF 终端分两行运行：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build/barge-in-d2 -p COM7 -b 115200 flash monitor
```

启动 ELF 应为 `008d03b334...`。本批只修改设备候选路径，不需要为这次 AC 门修复重启服务。80 PCM16 是未校准门限；低声真人可能漏检、高交流能量残余回声仍可确认，不能用主机测试代替设备效果，也不把历史三次误打断登记成已解决。

设备复测保持音量、摆位和风扇状态一致，使用本批独立 D2 固件：

1. 连接就绪、空闲时依次按 `a`、`b`，先让助手播报、全程不讲话；低 AC 候选应被拒绝，保留候选、`rejected_low_ac` 及观察日志。
2. 下一轮播报中主动说完整短句；确认真人讲话仍可进入 confirmed，并比较窗口 AC RMS 与门限。D1 模式下继续播报是预期行为。
3. 再按 `i` 验证真实插话：仅播报不应误停，真人讲话应停旧播报、收到匹配 ACK 并识别新句；若仍误停，提供相邻事件与是否讲话说明，不先把原因归于网络。

关联：[插话观察与诊断](p43d1_barge_in_observation.md)、[D2 交接](p43d2_barge_in_handoff.md)、[下行与 ACK 修复](p43d2_downlink_ack_fix.md)、[实时语音进度](../realtime_voice_progress_20261112.md)。
