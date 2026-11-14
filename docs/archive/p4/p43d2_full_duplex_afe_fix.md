# D2 播报起始误插话：全双工 AFE 配置修正

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

后续反馈：epoch `2194261324` 的 SDK 日志确认 FD／VERYAGGR 已启用，但扬声器接线断开、无人讲话仍误确认。这证明本页的配置修正不足以解决全部问题；新的输入直流与配对重试处理见[后续修正](p43d2_dc_and_pair_retry_fix.md)。本页原始构建身份与验证保留为历史。

更新：2026-11-11。设备附件 `545878cf-68b7-4c88-8a5d-b311b16e110f`，服务端 epoch `4243929666`。用户确认无人讲话，使用手抵住／堵住扬声器。该操作会改变声学路径，仍可能有残留声音或振动，不能当作电气静音对照。

## 本次在哪里判成人声

设备在约 81.882 秒为 turn 1 generation 1 开启播放期间观察。AFE 在 82.342 秒报告 `vad=speech`，观察器由候选进入确认，随后本地停播并发送取消。

| 事件 | 精确输出帧 | 实际确认统计 |
| --- | --- | --- |
| candidate，`at_us=81148689` | sample 1235840，mean 217，RMS 491，AC RMS 440，peak 1173 | 1 帧，AC RMS 440 |
| confirmed，`at_us=81302098` | sample 1238720，mean 361，RMS 3358，AC RMS 3338，peak 7761 | 连续 10 帧／200 ms，AC RMS **1600**，门限 **80** |

这次是设备端 AFE 输出被 VAD 标成人声，再被观察器确认为插话。`energy_rejections=0`：上一批 AC 门正常运行，但此次强交流信号通过了门限。它不是纯直流误报，也不能继续靠抬高门限解释或解决。

turn 1 正常识别 11 字。取消在服务端 received／cleanup_complete／ack_sent 均约 0 ms，设备 ACK 身份匹配，确认到 ACK 约 168 ms；随后误开的 turn 2 返回空识别，AC RMS 651、peak 8083。两轮服务端最大接收空档 94／125 ms、最大媒体落后 66／80 ms，没有本轮网络积压或取消超时证据。

确认时旧异步输入统计为 mic RMS 304／peak 342，输出精确帧交流能量却很强。这些输入与输出未对齐，不能据此计算 AEC 放大倍数，也不能证明参考直接串入麦克风。采集 overflow、参考 gaps／errors／unknown、分配失败均为 0，AFE ready／AEC active 且 error 0；数字时线健康不等于声学效果通过。

## 确认的配置缺陷

独立 D1／D2 固件仍沿用了 C 阶段的 `AFE_TYPE_SR + AEC_MODE_SR_LOW_COST`。设备实际流水线明确打印：

```text
AEC(SR_LOW_COST, NLP_OFF)
```

锁定的 ESP-SR 2.6.1 在 `esp_afe_config.h` 中区分 SR（不含 nonlinear suppression）和 FD（全双工，含 nonlinear suppression）。配置快照的 `nlp=1` 只说明参数值，不能证明 SR 流水线实际启用了 NLP。项目保存的小智参考 `build/xiaozhi-reference-review/main/audio/engines/afe_audio_engine.cc` 使用 `AFE_TYPE_FD + AEC_MODE_FD_LOW_COST + AEC_NLP_LEVEL_VERYAGGR`。

因此，可确认原配置没有运行全双工残余回声抑制；尚无受控对照证明它是此次误报的唯一原因。线性 AEC 的残差、声学路径变化、振动／电气干扰等声音来源仍待实机区分，不把“手堵扬声器”写成完全无声。

## 本批修改

- 编译 D1 或 D2 时，MR 前端使用 `AFE_TYPE_FD`、`AEC_MODE_FD_LOW_COST`、`AEC_NLP_LEVEL_VERYAGGR`。C-only AEC 观察与默认单麦配置继续作为 SR 基线。
- `afe_config_check` 后严格检查 type、AEC mode、NLP level，避免静默退回 SR；启动打印可读配置及 SDK 实际 pipeline，不能只凭 `aec_active=1` 验收模式。
- FD 显式设定 `vad_mute_playback=false`、`fixed_output_channel=true`、`output_playback_channel=false` 并检查：播放期间继续运行 VAD，输出固定为麦克风处理结果，不把参考声道作为输出，也不靠全程静音 VAD 来消除误报。
- 在已有 320 样本配对输入统计中增加 mic／reference 的 mean 和 AC RMS，候选、确认、拒绝及 `?` 日志保留。它们是最新输入观测，继续标记与输出未对齐；不用作插话硬门，不据此宣称 ERLE 或样本对齐。
- 保留 VAD 模型、NS／AGC 关闭、增益、数字参考时线、200 ms 确认窗、80 PCM16 AC 门及预录交接行为。

FD 的 NLP 是 AEC 的残余回声处理，与另一个 `ns_init` 噪声抑制开关不同。`NS=0` 不表示 FD 的 NLP 未启用。VERYAGGR 也可能压低弱声双讲，实机验证需同时检查误打断与真人讲话。

## 验证与复测

本批单麦／SR AEC／FD AEC 前端主机测试分别通过 27／39／43 个场景，控制器 80、观察器 32 个场景通过；覆盖模式降级、播放期 VAD 被静音、错误输出声道的拒绝，以及输入常量／带偏置交流能量、失败入队与满队列重试的统计边界。构建及主机 fake AFE 测试只验证配置、生命周期、音频传递和诊断，不运行真实 ESP-SR DSP，不代表本次声学问题已经解决。

独立 D2 已通过 ESP-IDF 6.1 构建，应用 2,769,744 bytes，3 MiB 应用分区余量 375,984 bytes，ELF SHA256 前缀 **`5565ae8796`**。已核对编译数据库、对象新旧时刻、应用内嵌 ELF 哈希及模型未变。验证报告为 FD 配置修正软件记录（历史生成产物：`build/p43d2_fd_verification.json`，当前目录未保留）。只重建本次 D2，默认／C／D1 二进制仍为历史版本。本次未烧录、未打开串口、未修改服务端，也无新固件实机验收结果。

首次全量重编译在未修改的 ESP-IDF `rgb_panel_draw_bitmap` 遇到 GCC 内部 segmentation fault；保留失败日志，降低为 4 个并行编译任务后通过，未修改 SDK 源码来绕过。该工具链重试与设备端误插话分开记录。

关闭原串口监视器，在 ESP-IDF 终端逐行执行：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build/barge-in-d2 -p COM7 -b 115200 flash monitor
```

先核对 ELF 前缀 `5565ae8796` 和启动的新配置标记 `profile=barge_in full_duplex=1 aec_profile=FD_LOW_COST nlp_requested=VERYAGGR`，并检查 SDK 的实际 `AEC(FD_LOW_COST, ...)` 流水线，不应仍是 `SR_LOW_COST, NLP_OFF`。连接就绪后按 `i`，在相同音量、摆位下分别测试：

1. 正常放开扬声器，仅助手播报，全程不说话；不应自动停播或误开下一识别轮。
2. 下一轮在播报中主动说完整句子；应停播、收到匹配取消 ACK，并正确识别插话。
3. 正常播完及按 `c` 后再讲话；检查下一轮首尾音、无旧声恢复。可另记手堵对照，但不要在单轮中反复改变声学路径来代替正常播放验收。

若仍误触发，提供第一次 candidate／confirmed 附近的 `barge_in_energy` 和启动 pipeline，注明是否讲话、是否手堵及扬声器是否正常发声。新输入 AC 仅作诊断，仍需结合场景判断。服务端本批未修改。

关联：[前批 AC 门及历史证据](p43d2_false_barge_in_fix.md)、[D2 交接](p43d2_barge_in_handoff.md)、[实施进度](../realtime_voice_progress_20261112.md)。
