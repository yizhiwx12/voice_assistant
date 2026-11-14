# 实时语音实施进度

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../operations.md)，最新状态请查[当前进度](../realtime_voice_progress.md)，返回[文档索引](../README.md)。归档不改变未验收事项。

更新：2026-11-12。

## 当前进度与下一步

本轮最新反馈：用户确认扬声器恢复供电后 D2 也正常；该基本功能记录沿用。当前处理正常自动识别对短促动静过敏、开轮后空识别的问题：已增加独立 160 ms 起音确认与 AC RMS 80 持续性门，至少 4／8 帧达标、尾部 2／3 帧达标，确认前只本地缓存，保留完整 400 ms 预录。主机起音 30 场景、控制器、D2 33 场景、UI 132 项及前端／音频参考回归通过；固件构建、烧录和新起音门的实机效果待用户验证。当前仍用根 `sdkconfig`／`build/barge-in-test`，需重新构建／烧录本批固件，操作及边界见[正常监听起音修正](p4/p4_idle_onset_filter_20261112.md)。之后继续原 P5 规划。

此前针对短促外部声音误触发的 D2 能量持续性修正后，用户反馈“感觉效果还行”。记为 D2 基本效果获用户认可；本轮未附新日志，不补记精确停播时延、误触发率或完整声学验收通过。D2 保留 200 ms 确认窗和 AC RMS 80，确认窗至少一半且不少于 3 帧达到能量门限，并要求最后 3 帧至少 2 帧达标。本次正常起音门只应用于空闲监听，D2 已确认交接不重复等待。

当前使用根 `sdkconfig` 与 `build/barge-in-test`，保留 QIO／80 MHz 和现有调度，运行时按 `a` 开启正常自动监听或按 `i` 同时开启 D2。下列旧固件、命令与故障结论保留为历史，不切换旧构建目录。

阶段顺序：已确认的 D2 基本效果和正常扬声器结果直接沿用，本次先验证新的正常监听起音门，再进入总规划 P5「集成与选择体验」：优先显示当前 ASR／LLM／TTS 组合并完善独立选择与新会话生效边界，再补重连、长时资源及分段时延统计。不重做整套 D1 或已跳过的 B 验收；唤醒词／离线命令为后续可选功能。

本次已修改自动起音固件代码、配置、诊断及回归测试，未运行固件构建、烧录、串口或服务启动。跨音量／距离、声学校准、长期稳定性及既有网络专项继续留档。操作见[本批说明](p4/p4_idle_onset_filter_20261112.md)，阶段顺序见[总规划](realtime_voice_plan_20261112.md#10-分阶段实施与验收)。

## 历史排查与交付记录

最新反馈：epoch `3013152528` 仍在扬声器接线断开、全程无人讲话时误触发插话；turn 2 返回空识别，取消 ACK 与服务端媒体指标正常。此前 FD／VERYAGGR 与连续去直流均已在设备运行，不能继续归因扬声器回声或网络，也不能把前几批修正记为解决。已完成独立真实 DSP 隔离诊断固件：启动前依次运行五组已知输入，每组使用全新 AFE，完成后才启动正常音频；同步增加 FD 输入源的 ordinal 统计窗口，仅用于事件取证。软件测试通过，固件尚未烧录，尚无真实 DSP 结果。

本批软件交付见[真实 DSP 隔离诊断](p4/p43d2_signal_isolation_diagnostics.md)及验证报告（历史生成产物：`build/p43d2_signal_probe_verification.json`，当前目录未保留）。诊断 ELF、五个 case 的设备日志和 source window 才能区分 AFE／参考／输入电气干扰；`status=completed` 只表示统计完整，不表示声学通过。下一步只需烧录 `build/aec-signal-probe`，提供 `afe_signal_probe begin`、五个 `case=` 和 `afe_signal_probe end` 日志；无需重复完整插话验收。

最新反馈：epoch `2194261324` 在扬声器接线断开、无人讲话时仍误确认；设备已实际运行 `FD_LOW_COST, NLP_VERY_AGGRESSIVE`，因此 FD 配置修正尚未解决误插话。确认窗 AC RMS 99 高于原门限 80；最新原始输入 mean 292／AC RMS 13 提示明显直流偏置，但输入输出未对齐，不能认定它是唯一根因。另一段 turn 4 快照为 `pair_age=208 ms`，AFE 曾满队列、参考完整且采集无溢出，与误确认分别处理。本批补充 D1／D2 麦克风连续去直流，修正 AFE 暂满后空闲后台仍可能等 20 ms 的重试遗漏，并将观察关闭的 AEC／会话原因分开记录。详见[去直流与配对重试修正](p4/p43d2_dc_and_pair_retry_fix.md)。软件与实机验收状态以该记录为准，当前仍未通过声学验收。

本批 mono／SR AEC／FD AEC 前端 33／45／49、控制器 81 场景及 AudioService 两种配置回归通过；独立 D2 的 ESP-IDF 6.1 构建通过，ELF 前缀 **`f5f7d7f03b`**，应用 2,770,432 bytes、分区余量 375,296 bytes。已核对新头文件编译依赖、对象时刻、内嵌 ELF 哈希与模型；报告为去直流与配对重试软件验证（历史生成产物：`build/p43d2_dc_pair_verification.json`，当前目录未保留）。未烧录、未打开串口、未改服务端；无本批设备复测结果，默认／C／D1 二进制未重建。

以下为前批 FD 配置修正的交付记录：epoch `4243929666` turn 1 在无人讲话、手堵扬声器时仍误确认，连续 200 ms 的精确输出 AC RMS 为 1600，高于门限 80；上一批低 AC 门未解决此类误报。取消 ACK 匹配且约 168 ms 返回，服务端无本轮积压证据。核查发现 D1／D2 仍使用 C 阶段 SR AEC，实际 pipeline 为 `SR_LOW_COST, NLP_OFF`。随后将独立 D1／D2 改为全双工 FD_LOW_COST／VERYAGGR，并补充最新配对输入 mean／AC RMS 诊断。模式缺陷已修正，但不能将它写成声学误报已经解决。详见[全双工 AFE 修正](p4/p43d2_full_duplex_afe_fix.md)。

本批前端单麦／SR AEC／FD AEC 27／39／43、控制器 80、观察器 32 个主机场景通过，独立 D2 的 ESP-IDF 6.1 构建通过；ELF 前缀 `5565ae8796`，应用 2,769,744 bytes、分区余量 375,984 bytes。报告见FD 软件验证（历史生成产物：`build/p43d2_fd_verification.json`，当前目录未保留）。本次未烧录或打开串口，无新固件声学验收；默认／C／D1 二进制未重建。

以下 AC 门交付及验证为前批历史，不能作为本轮设备通过结论。

本轮用户已确认 epoch `591496209` 的 turn 5、7、12 播报时均无人讲话，三次属于实际误打断；新轮 6／8／13 的空识别不能归因于用户小声。三次取消 ACK 均匹配，确认至 ACK 约 213～224 ms，未重现旧取消超时。旧 epoch `2658432150` 的 1484 ms 上行空档为独立故障。当前已在 D1／D2 连续 VAD 确认窗口补充 AC 活动条件：按精确 320 样本输出帧去均值并累计约 200 ms 窗口能量，低于默认 80 PCM16 RMS 时拒绝打断，进入冷却；不逐帧截断弱音。80 为未校准工程初值，足够能量的残余回声仍可能通过，设备效果待验。详情及新日志见[安静播报误打断修复](p4/p43d2_false_barge_in_fix.md)。

本批候选观察器 32、控制器 80、自动发言缓存 20 个场景通过，包含直流误报不取消、带偏置的有效声音确认、弱音跨帧和原 PCM／预录完整交接。去均值只用于统计，不修改波形、空闲自动监听、手动录音、AEC DSP 或网络预算。固件身份及设备复测状态以[本批修复记录](p4/p43d2_false_barge_in_fix.md#软件验证与设备验收)为准。

本批独立 D2 配置生成及构建已通过，ELF 前缀 `008d03b334`、应用 2,768,432 bytes、分区余量 377,296 bytes；配置确认 AC 门限 80、确认窗 200 ms。已核对编译数据库、对象时刻、应用内嵌 ELF 哈希及未改前端文件／模型。报告为AC 活动门软件验证（历史生成产物：`build/p43d2_acgate_verification.json`，当前目录未保留）。COM7 已发现，本次未打开串口或烧录，仍需本批设备复测；旧 `1c5d5b6b06` 为只加诊断的固件。

此前诊断批：在 AFE 配对输入与控制器事件边界增加麦克风／数字回放参考／AFE 输出 RMS 和峰值，保留候选与确认时刻、人声帧数；明确标记未做样本精确对齐。该批只加诊断，未调整 AEC、VAD 阈值或打断行为。历史步骤与指标边界见[D1 插话误触发诊断](p4/p43d1_barge_in_observation.md#2026-11-11-插话误触发诊断补充)。

该诊断批软件验证：前端单麦 26／AEC 35、控制器 78、候选观察器 24 个场景通过；独立 D2 构建通过，ELF 前缀 `1c5d5b6b06`，应用 2,766,816 bytes、分区余量 378,912 bytes。已核对编译数据库、对象时刻和应用内嵌 ELF 哈希，详见诊断验证报告（历史生成产物：`build/p43d2_energy_verification.json`，当前目录未保留）。该报告生成时未发现串口、未烧录，随后用户提供了上述三次误打断反馈。旧能量是独立原子观测且输入／输出未对齐，不能直接计算回声消除量；重复事件快照须按身份、时刻与样本位置去重。

更早 D2 复测曾有两次取消超时，新增服务端日志证实那两次取消接收、清理和 ACK 写出约 0～1 ms；设备未记录匹配确认，约 2 秒后主动关闭。随后限制服务端下行突发至 200 ms 并按媒体速率发送，避免提前灌入大量旧音频；设备补完整收包／入队／分发／匹配诊断。旧音频占用同一 TCP 顺序是有证据支持的风险，不宣称唯一根因；上述最新三次交接已见匹配 ACK，长期复测继续保留。历史修复见[下行节奏与确认修复](p4/p43d2_downlink_ack_fix.md)。另一次 epoch `3416088149` 上行空档 1344 ms／媒体落后 1316 ms 触发原保护，传输或设备调度原因待区分；本地 AEC／参考健康。

此前首次 D2 复测四次确认中两次成功交接、两次取消超时；一轮交接后高能量短音频及后续四轮低能量录音为空文本，分别保留，不统一归因网络或小声。上一批已修复 TTS 快速取消、抢占正常关闭及流水线取消顺序，服务端 232 项测试通过，见[取消清理修复](p4/p43d2_cancel_timeout_fix.md)。上述两次新失败不能再归因于旧清理慢，先前修复及历史结果保留。

用户已明确接受当前 D1 效果并要求继续，当前进入[D2 自动插话与音频交接](p4/p43d2_barge_in_handoff.md)。这属于按用户决定推进功能，未将此前偶发误确认和未做的声学项改记为通过。D2 独立编译、开机关闭，`i` 显式开启自动监听与实际插话；`a` 回半双工，`b` 回仅观察，`r/m/c/n` 关闭 D2。实现以原回复身份封轮、先本地停播、等待完整身份匹配的取消 ACK，再把确认前 400 ms 预录和持续讲话接入新轮；确认到新流开启共用 2 秒预算，上行保持 20 ms 节拍。独立固件目录为 `build/barge-in-d2/`，用户已运行并提供上述首次反馈；完整实机验收及本次服务端修复效果仍待确认。

后续五轮 D1 复测（epoch `2145995825`）已有两轮正常无触发区间：第 3、4 轮约 25.8／32.3 秒播报期间候选和确认均未增加；第 1、2、5 轮分别新增 3／1／2 次确认。用户仅记得一轮主动插话，具体轮次不明，因此本轮不能确认全部仅播报误触发已消失。五轮上行正常结束，AEC／参考快照健康；D1 完整验收及此前偶发误确认继续保留；其后用户接受当前效果并选择进入 D2。详见[后续五轮复测](p4/p43d1_barge_in_observation.md#后续五轮复测部分播报未触发候选)。

本轮 D1 实机反馈已记录：正常播报时不再出现此前的争锁“不可用”，真人在播报中讲话可从候选进入确认，但仅扬声器播报仍可能误确认。该次未通过完整 D1 验收；其后用户已选择继续 D2，未决声学项保留。另一次上行中止（`epoch=4184591717 turn=6 stream=6`）已取得配对服务端首错：服务端观察到 1360 ms 接收空档、1285 ms 媒体时间线落后，超过 750 ms 保护阈值；故障时队列为空、入队等待为 0，事件循环无明显延迟。可确认媒体到达服务端过晚，尚不能区分设备上行调度与网络传输路径。设备重连后后续两轮正常结束上行。详见[D1 本轮实机复测](p4/p43d1_barge_in_observation.md#本轮实机复测)。

[D1 普通状态锁争用修复](p4/p43d1_controller_busy_fix.md)以独立原子回复修订号隔离旧候选，AFE 回调及活动观察无需获取状态锁。初版 ELF `9852804f87` 在 93.362／142.110／240.471 秒三次因 `controller_busy` 关闭观察，初版硬件记录（历史生成产物：`build/p43d1_first_hardware_observation.json`，当前目录未保留）保留。修复批候选 19、控制器 54、UI 状态／文字存储 121、自动缓存 14 项及三套构建通过；该历史修复批默认／AEC／D1 构建 ELF 分别为 `d63bdbbeff`／`3d404d4c5c`／`0ade40c341`，见修复软件报告（历史生成产物：`build/p43d1_busy_verification.json`，当前目录未保留）。`b` 仍独立开启 D1 观察，`a` 仍半双工。

初版D1交付历史：400ms预录、200ms连续确认、2秒有界保留，候选只显示统计、不上传／停播；19／44／121／14主机检查及三构建曾通过，默认／AEC／D1 ELF为`54b01ca62d`／`5acb4b55af`／`9852804f87`，软件报告为初版交付（历史生成产物：`build/p43d1_verification.json`，当前目录未保留）。保留其生成时未烧录事实，随后设备反馈另记，不覆盖为当前验收通过。

上一轮 C 观察：用户已完成[播报期间本地检测显示](p4/p43c_playback_observation_ui.md)的设备观察，并确认“一切正常”：自己保持安静、仅助手播报时“都是静音”；播报中同时讲话时“有转化为人声，说完回复静音”。这两项屏幕基本观察登记为用户主观通过，结合此前近端正常识别且首尾无损、正常播完／按 `c` 后下一轮正常的反馈，C基础运行与检测观察正常。播放期间仍不会自动打断。该批UI状态89项、会话生命周期30场景及默认／AEC双构建通过；交付身份为默认ELF `0eb4471748`、AEC ELF `e2c8a3538d`，未提供新启动日志，不另记为ELF身份复核。多音量／摆位对照、双讲词句保真、定量声学、标定及长期资源仍分别保留验收，`calibrated=0`，B跳过项保留。该次反馈仅更新文档，没有修改代码、重新构建或执行烧录；原软件报告保留生成时点状态。

此前日志基线：**第八批AEC固件已实际运行，基本功能本轮正常，用户确认“一切正常”。** 附件`78018aa0-d596-406b-ac02-151ee0403406`启动ELF前缀`17284ff14`与第八批`17284ff146`匹配；日志记录至120.034秒，未见E级错误、看门狗、`first_failure`或AEC失效，turn 1～4均正常结束上行。AEC／参考健康、供给调度和资源已有本轮样本，91.351秒停播栅栏后turn 4仍成功；日志未标明完整`c/a`按键顺序，不能据此勾选全部取消和长播专项。详细问题、修复过程与本轮证据见[AEC问题复盘](p4/p43c_aec_issue_retrospective.md)及[第八批修复](p4/p43c_playback_stutter_fix.md)，硬件记录为`build/p43c_eighth_repair_hardware_observation.json`。C基础稳定反馈已登记，远端／近端／双讲等受控声学效果和标定仍未验收，`calibrated=0`，D关闭，B跳过项保留。

当前默认实现：开机为手动模式；串口`a`开启自动监听，`m`切回手动，`r/s`保留原始PCM手动基线。自动路径使用AFE连续输出、目标400ms预录、2秒有界握手缓存和700ms静音判停；无人讲话不创建云端回合，已有发言保留30秒上限。识别／生成／实际播放时暂停自动起轮，恢复需先观察300ms静音。服务端已有`speech.cancel`／`speech.cancelled`，取消清理上限5秒，保留回合封存和迟到数据隔离。C选项默认关闭AEC；显式开启后只观察AEC，播放期仍不自动起云端轮或取消回复。

进入C前的用户反馈：正常音量下能够识别，少数字因气短、音量小而偶尔漏字，用户认为可接受，其他功能无问题。本轮不据此调整增益、云模型或自动判停阈值。400ms预录已进入`sdkconfig`、`sdkconfig.defaults`及生成配置，自动发言缓冲14项、前端生命周期18项和此前ESP-IDF构建通过；这些属于进入C前的验证，不代表AEC通过。

进入C前的设备日志：附件`58c24664-c7eb-463b-aaa9-f38f3536ef05/已粘贴的文本.txt`中的ELF前缀为`d6ba892eb`，属于进入C前的历史证据；后续用户已运行C旧版`df87055e6`，本批修复版为`124091721f`。普通VAD／语音上行工作，多轮结束正常；但`capture_timeline valid=0 overflow=11`、`acoustic_delay_probe attempts=0`，各播放作用域也无候选。溢出计数首次`?`约13.6秒时已为11，此后未增加，准确发生时刻／触发条件尚未隔离；该日志不具备声学校准或AEC输入健康的验收证据。资源约内部73.5KB、PSRAM7.6MB、分配失败0，capture／playback栈最低剩余1832／2396字节。用户决定不做本轮B／溢出专项验证并直接实施C；这些结果不被改记为已修复或验收通过。

B收尾软件历史：按播放保留的通过候选／分布／拒绝统计、参考任务耗时分项与栈优化主机检查及构建曾通过；当时应用2731648字节、bin SHA256 `4bb009a5892b3160c8a1f539f1584c0267099f74e0c5845bd7d0c499977311ad`、ELF前缀`4e0b0163e7`，报告`build/p43b_stats_verification.json`、日志`build/p43b_stats_build.log`。这是历史软件产物，不是最新设备运行版本或C产物；该统计批候选分布／主观取消／新任务资源硬件结果仍未验收，见[B收尾记录](p4/p43b_acoustic_statistics_acceptance.md)。

已有实机基线：此前用户确认安静环境手动功能、30秒空识别／到期提交播报及长时待机正常，未提供精确待机时长。后续自动路径同一连接 `epoch=953293006` 已记录turn 1–10：1–4、9、10有文字，5–8为空；空结果后同连接后续轮次恢复识别。turn 11出现上行到达空档1391ms／媒体落后1273ms而中止。此为现场序列，不是准确率基准；用户现反馈中文识别较好、英文较弱。关掉风扇后恢复识别的历史开关对照继续保留，具体气流／运行声／振动影响未分离。详细轮次、证据与暂缓事项见[P4实机记录](p4/p4_local_audio_acceptance.md#p42-本轮实机记录与后续安排)和[P4总结](p4/p4_issue_summary.md)。

上一版B软件验证：生产参考10组、重采样10组、麦克风DMA6组、声学相关5组、monitor集成6组、时线分数映射／64位边界／模拟正负250ppm漂移及驱动100次重建／14项失败恢复等检查通过；汇总见`build/p43b_verification.json`。ESP32-S3／IDF6.1构建通过，新源已核查编译数据库／对象／链接，RX／TX回调在IRAM，麦克风无64位原子helper调用。当时应用2,728,768字节，3MiB分区余约13%，SHA256为`08b421e7eefbed891822978c3d35bae53246ad526389aa25fa9317679dedfadb`；模型288,069字节，SHA256为`2485a4caf5754547459622a0ba126428c3dab1cef8bf57ba24e463586d2232ee`，构建日志`build/p43b_build.log`。软件报告的`flashed=false`是当时交付状态；随后用户运行证据另记于`build/p43b_hardware_observation.json`，不覆盖历史软件报告或冒充新批验收。

上一版B实机：附件`10a5e0f5-3a4b-43a9-81b2-14a6ebd8c237/已粘贴的文本.txt`覆盖约852.625秒；应用描述`Nov 10 2026 19:55:49`及ELF前缀`77025a4c7`与该版bin描述一致。generation 1在AFE初始化前积压54块、超过8槽丢弃46块，随后已明确重建封代；generation 2后队列丢弃／格式／左右不匹配／异常连续性计数均为0。末次RX有效且无溢出／invalid，重采样20330640→13553760严格3:2、errors／overwritten为0；mapped84726、unmapped0、healthy1，历史30块／84696覆盖是正常滚动。数字完整帧21760／21789（约99.867%），有效样本6967527／6972480（约99.929%）；unknown15／gaps14是显式边界。mic／tx拟合为1／−1ppm；max_poll69095µs接近80ms事件槽容量，栈剩余最低capture1176／playback2220字节；这些是旧版结果，新统计批仍待设备采样。

B资源初／末快照internal约73807→73579、largest31744、min39828；末次DMA65791、min32040；PSRAM7645752→7645760、min7575180，分配失败0。支持本次资源观测通过，不能证明长期无泄漏或AEC共存。声学attempt376／accepted26／rejected350、capture_drop0，但`?`最近帧均未通过，缺少已通过延迟的分布；被拒绝的720样本／相关377不能换算成45ms校准。软件停播栅栏37951／34830µs不证明主观停声或旧声不恢复。唯一断连epoch4195377106／turn11的发送最大224233µs、队列11、write0／errno5后errno9，缺少配对服务端日志，不能定案为Wi-Fi抖动；空结果缺少`voice_asr_result`／`pcm_ac_rms`，小声仍为假设。完整边界见[B批设备记录](p4/p43_reference_alignment_acceptance.md#本轮b批实机记录)。

A批历史软件验证：参考10组、驱动100次重建／14项失败恢复和控制器30场景通过，当时应用2,712,640字节，SHA256为`b392563f253eecd31c77ed718ee1f92ae46703a625acb7c488d47d3d2762f265`。随后设备日志验证参考计数及约495秒资源快照、未见分配失败；声学听感及特定取消不能由计数完全证明。A历史证据为`build/p43_build_final.log`、`build/p43_*_tests.log`和[A批实机记录](p4/p43_playback_reference_acceptance.md#本轮实机日志记录)。本批未改服务进程或调用云API。

此前P4.2软件验证：服务端209项测试通过、无跳过；控制器30场景、自动发言缓冲13场景、AFE生命周期18场景、UI89项、PCM适配5场景／6种块长通过；当时应用为2,709,536字节，SHA256为 `00b3290def4d30d8d3d394a4fa1d8b53ea9c7814ee8c781fda5443f446e7d412`，日志为 `build/p42_build_final.log` 和 `build/p42_server_tests.log`。本批没有服务端变更，未重跑该服务端历史测试集。

P3 阶段的问题、修复、验证边界及未解决事项已汇总至 [P3 问题与解决方法](p3/p3_issue_summary.md)，包含后续上行到达停顿和 Pong 超时，避免将其与已修复的 ASR 等待或待机策略混淆。

前轮修复已将AFE恢复为ESP-SR默认50帧内部容量，入口160ms队列保持不变；有界后台日志隔离终端输出，当时192项服务端测试通过，后续空结果诊断回归为203项，均为历史验证记录。前轮证据见 [AFE/服务端停顿记录](p4/p4_frontend_ringbuffer_and_server_stall.md)。已锁定 ESP-SR 2.6.1 / `vadnet1_medium`，模型分区、构建清单、启动 SHA256 校验和持续AFE/VAD已有实机证据。此前 LVGL 64 KiB 固定池迁至 PSRAM、普通分配偏好与内存诊断实现见 [内存修复与验收](p4/p4_memory_budget.md)。P4.2自动模式现已接入AFE输出并重组为20ms上行帧，手动原始PCM路径继续保留。操作和部署见 [P4 部署与验收](p4/p4_local_audio_acceptance.md)，勾选项见 [P4 清单](p4/p4_local_audio_checklist.md)。P3 完整验收和网络停顿／Pong 超时问题继续保留；服务端历史全局停顿已确认，但其唯一来源尚无现场线程栈证明。

P1、P2已完成默认组合实现与小样本验证；P3固件实时录放、WebSocket、Opus、状态界面和串口手动控制已接入。用户反馈新版长时待机未再报错；风扇背景声录音在30秒边界失败曾由`asr_reason=receive_timeout`确认是本地ASR响应等待到期，已改为成功音频发送进度保护及独立EOF最终结果预算。该轮183项服务端测试属历史记录。GPIO8、errno、LCD回调及文字分页修复保留，P3完整验收仍未通过。细节见[断连问题与处理方案](p3/p3_recording_disconnect_issues.md)，P2证据见[P2云端验收](p2/p2_cloud_acceptance.md)。

## 当前实现范围

| 模块 | 已实现内容 | 仍待验证或实施 |
| --- | --- | --- |
| 配置与选择 | 不可变配置快照、profile、各层独立覆盖、能力登记、选中服务校验、环境变量优先；已填写本地火山业务配置 | 其他可选模型/地域/音色；运行期图形选择体验 |
| 旧 HTTP | `/ping`；完整 PCM16 mono16k 的 `/audio`；两行文字响应；原入口兼容 | 真实 Qwen/DeepSeek 账号回归 |
| ASR | Qwen 整句兼容、豆包流式 ASR 2.0；partial 与整轮提交分开；手动与自动路径已有中文实机识别证据 | 英文识别、偶发短候选空识别、弱音及风扇鲁棒性暂缓优化 |
| LLM | 可配置 Chat Completions；HTTP 和 SSE；只提取回答文本；DeepSeek 真实 SSE 已通过，截断明确失败 | Qwen 所选模型及地域；更广泛问题和时延统计 |
| TTS | 火山 v3 双向流式、CosyVoice 双向流式；规范输出 PCM16 mono24k；火山 Vivi 2.0 真实合成与格式检查通过，安静环境设备播报正常获用户确认 | 其他音色及 CosyVoice 权限；完整听感／首尾音专项验收 |
| WS | `/ws/voice`；会话/回合/回复编号；32 字节二进制头；25 项有界队列、100 ms 入队等待、750 ms 排队/相对媒体落后检查、取消隔离、最老音频/worker 阶段/本轮与连接指标；默认长期待机、可配置正常空闲关闭 | 真实网络抖动、Wi-Fi 延迟来源及长时运行；默认预算待实机复验 |
| Opus | 默认 20 ms raw packet，上行16k/24kbps、下行24k/32kbps；设备真实 ESP-Opus wrapper 主机交叉编解码、首尾裁剪检查通过 | Xtensa 负载、设备编解码/播放、真实录音准确率 A/B |
| PC 工具 | 同连接多 WAV、等待 ASR 就绪、首音频后取消、逐轮 WAV/JSON 时序与字节报告、严格失败判断；真实链路通过 | 真实麦克风与长时试验；不代表扬声器已播放 |
| 设备前端 P3 | ICS-43434 持续采集 16 kHz、MAX98357A 24 kHz 播放、Opus/PCM、取消时清队列和重建 TX DMA；SD 生命周期分离；UART0 `r/s/c/n/?`；errno 按类型读取；LCD 回调单一归属；8 KB 文字保留、每页三行、5 秒自动及 `[`/`]` 手动翻页 | 新增显示与诊断待实机复验；首尾音、连续回合、取消隔离、DMA 停播、电源与 RAM/PSRAM 水位待完整验收 |
| 本地语音 P4 | P4.1／P4.2 中文、A／B 数字参考基线及 C 基础运行保留；D1 观察和争锁回归已有设备反馈；D2 已加入能量持续性保护，用户反馈修正后基本效果可接受 | 现固件简短确认正常扬声器下误触发、插话首尾／旧声隔离与连续多轮；完整声学／标定、长期资源和 B 跳过项保留，随后进入 P5 |

适配器“已实现”不表示任意填写的模型 ID 都有效。当前豆包/DeepSeek/火山默认组合已完成实际整链路；可选 Qwen LLM 与 CosyVoice 仍保留占位符，选中时会拒绝启动。未选中的配置不要求凭证齐全。实时 LLM 默认预算为 1024 token，非流式保留 128；可用业务配置 `max_tokens` 覆盖。SSE 的 length 等非正常结束会报告回复失败，避免将截断当成成功。

## 文件职责

- `server/voice_server.py`：启动入口；`start_voice_server.ps1`：Windows 参数转发与 Python 环境选择。
- `server/core/`：配置、能力注册、会话历史、ASR → LLM → TTS 编排、稳定文本分段。
- `server/providers/asr/`、`llm/`、`tts/`：厂商协议；密钥始终保留在服务端。
- `server/transport/`：旧 HTTP、实时 WS、音频帧与连接生命周期。
- `server/audio/`：PCM/WAV 及 libopus 原生绑定。
- `tools/voice_client.py`：PC WAV 客户端；输出放在 `server/data/`。
- `main/audio/local_audio_frontend.*`、`pcm_block_adapter.hpp`：连续 AFE/VAD、带样本位置的处理结果、块长适配和任务生命周期。
- `main/audio/automatic_utterance.hpp`：有界预录／握手缓存、讲话和静音端点、连续样本时间线；由控制器管理回合和发送节奏。
- `main/audio/sr_model_store.*`、`tools/prepare_sr_model.py`：运行时模型核验与构建部署清单；镜像和生成头文件保存在 `build/`。
- `server/tests/`：旧行为回归、配置/CLI、协议/适配器、编排和本机音频互通测试。

固件继续保持 `main/app`、`audio`、`network`、`display` 的职责分类，本轮没有将服务逻辑塞入设备入口。

## P3 固件实现与交接

修复前实机反馈为持续录音几秒后“上行语音发送失败”。ASR 节流已改为累计 PCM 样本的绝对 deadline，并限速追赶；GPIO8 保留登记和低电平完成静音 DMA 重建；两端已保留首因并记录耗时、队列、关闭码和资源水位。30 秒 Windows 实时钟 mock 回归队列峰值 2/25，确定性抖动回归峰值 9/25；GPIO 生命周期主机回归通过。现场首因及设备处理能力仍需新固件实测确认，详见 [问题与处理方案](p3/p3_recording_disconnect_issues.md)。

- `main/audio/audio_service.*` 持续把 I2S 麦克风转换为 16 kHz PCM，独立任务以 24 kHz PCM 播放；播放队列最多约 1 秒。打断同步清空应用队列并重建 TX DMA。
- `main/audio/opus_codec.*` 使用锁定的 `78/esp-opus 1.0.5`；上行 16 kHz、下行 24 kHz，raw Opus、20 ms mono 包，PCM16 仅作为显式配置的调试回退。
- `main/network/voice_client.*` 和 `voice_protocol.*` 实现有界 WebSocket 消息队列、协议头校验及 TCP 背压；`main/app/conversation_controller.*` 管理回合、文本、回复取消和重连。
- 串口监视器连接 UART0/115200：开机手动模式，`r` 开始录音，显示录音后再说话；`s` 结束并提交。P4.2增加 `a` 开启自动监听、`m` 切回手动；`c` 取消当前录音／回复并停止自动监听，`n` 重连、`?` 打印堆/音频指标，`[`/`]` 文字翻页。自动模式在识别／生成／播放期间暂停起轮，未启用播放中自动插话。
- WebSocket URL 可由 `CONFIG_VOICE_WS_SERVER_URL` 显式设置；留空时从 HTTP base URL 推导 `/ws/voice`。地址需指向局域网可达的服务端；设备不保存云厂商密钥。默认服务 profile 是 `realtime_default`。
- 复核并补上 UART0 命令输入所需的 RX 驱动初始化；修复重连与旧录音启动任务的同步，并在回复开始前失败或取消时正确退出等待状态。
- ESP-IDF 构建信息确认新增的音频/网络/控制器 `.cpp` 都进入 `compile_commands.json`，且对应对象晚于源文件。协议检查 21 组，Opus 检查 14 组（双向、16/24 kHz）；它们是主机交叉检查，不代表 Xtensa 性能或 I2S 硬件验收。
- 此前 P3 ESP32-S3 修复构建通过，当时 3 MB 应用分区剩余约 34%；历史日志在 `build/p3_stability_final_build.log`。P4 新增 AFE 库后的最新产物和容量见 [P4.1 部署与验收](p4/p4_local_audio_acceptance.md)。Wi-Fi 默认禁用省电，`VOICE_WIFI_POWER_SAVE` 可对比；发送预算 `VOICE_UPLINK_SEND_TIMEOUT_MS` 默认保持 200 ms。
- P3当时服务端完整离线测试166项通过、无跳过；VoiceClient及控制器各7组主机检查、文字/UTF-8 67项通过。`realtime_default --check-config`通过，业务配置补上 `transport.idle_timeout_seconds=0`。这些是历史基线，本轮P4.2最新回归数量见本文开头。

硬件连接参考现有固件配置：ICS-43434 使用 I2S1，BCLK/WS/DIN 为 GPIO4/5/6；MAX98357A 使用 I2S0，DOUT/BCLK/WS/SD 为 GPIO15/16/17/8。实际载板原理图和复用情况未确认，烧录前应核对接线。操作、指标含义和逐项验收记录见 [P3 真机验收](p3/p3_device_acceptance.md)。所有硬件结论仍以用户设备实测为准。

## 配置与可用命令

以下 PowerShell 命令从项目根目录执行。入口优先读取已创建的 `server/config/voice_config.json`；不存在时读取 `voice_config.example.json`。默认 `legacy_http` 保留原启动行为；实时组合通过 `--mode realtime --profile realtime_default` 选择。用户填写的火山密钥已通过配置加载和真实调用检查，检查过程不显示密钥值。

本地配置及模板明确填写：豆包 ASR 的 `endpoint=wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async`、`model=bigmodel`、`resource_id=volc.seedasr.sauc.duration`；火山 TTS 的 `endpoint=wss://openspeech.bytedance.com/api/v3/tts/bidirection`、`resource_id=seed-tts-2.0`、`voice=zh_female_vv_uranus_bigtts`。`qwen_default` 和 `cosyvoice_default` 保留为可选配置，使用前再填写实际地域、模型和匹配音色。

密钥文件负责凭证，业务配置负责选择服务和模型。ASR 的版本由 `resource_id` 区分：`volc.seedasr.sauc.duration` 为 2.0 小时版，`volc.seedasr.sauc.concurrent` 为 2.0 并发版；`model=bigmodel` 是请求体必须使用的名称。TTS 的 `resource_id=seed-tts-2.0` 已明确选定语音合成 2.0。标准音色无需另写 TTS `model`，云端默认版本为 `seed-tts-2.0-standard`；官方文档仅要求复刻音色场景显式指定该参数。音色本身是必选参数，没有可省略的统一默认值，当前选择 Vivi 2.0 作为本项目默认音色。

参数依据：[豆包双向流式 ASR](https://docs.volcengine.com/docs/doubao-voice/bidirectional-streaming-automatic-speech-recognition-websocket?lang=zh)、[豆包双向流式 TTS](https://docs.volcengine.com/docs/doubao-voice/bidirectional-streaming-text-to-speech-websocket?lang=zh)。

凭证文件沿用 `server/config/server_secrets.json`，示例中新增 `VOLCENGINE_ASR_API_KEY`、`VOLCENGINE_TTS_API_KEY`。各层通过 `credential_ref` 选择凭证，环境变量优先；旧 `DASHSCOPE_WORKSPACE_ID`、`DASHSCOPE_ASR_MODEL`、`DEEPSEEK_MODEL` 继续兼容。

火山新控制台的推荐鉴权使用 `X-Api-Key`，ASR 与 TTS 分别使用对应凭证及资源 ID。火山 TTS 可选 `app_id_ref` 支持旧鉴权；未配置时不要求 App ID。账号开通 ASR 不自动授予 TTS 或任意音色权限。

```powershell
# 准备运行环境（已有 .venv-voice 时只需按需更新依赖）
python -m venv .venv-voice
.\.venv-voice\Scripts\python.exe -m pip install -r server/requirements.txt

# 无需凭证即可列出模型及当前能力；不读取密钥文件
.\.venv-voice\Scripts\python.exe server/voice_server.py --list-options

# 配置检查：不绑定端口、不调用云；实时 Opus 配置还会加载原生库
.\.venv-voice\Scripts\python.exe server/voice_server.py --mode http --profile legacy_http --check-config
.\.venv-voice\Scripts\python.exe server/voice_server.py --mode realtime --profile realtime_default --check-config

# 启动已配置的豆包 ASR → DeepSeek → 火山 TTS 组合
.\.venv-voice\Scripts\python.exe server/voice_server.py --mode realtime --profile realtime_default

# 独立覆盖，不改写 profile 原定义
.\.venv-voice\Scripts\python.exe server/voice_server.py --mode realtime --profile realtime_default --llm qwen_default --tts cosyvoice_default
```

启动脚本优先 `.venv-voice`，再回退 `.venv-asr-fixed`、PATH Python。本机系统执行策略会拦截直接运行 `.ps1`，可只对本次进程指定方式：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\server\start_voice_server.ps1 -Mode realtime -Profile realtime_default -CheckConfig
```

`--config` 与 `--secrets` 可指定其他文件；脚本对应 `-Config` / `-Secrets`。

`--check-config` 验证结构、所选能力、占位符、必需凭证是否存在和原生 Opus 加载；不会校验密钥有效性、实际模型权限或云端可用性。切换配置需新会话；修改磁盘文件需显式重启生效。HTTP 模式要求 `--tts none`，不会悄悄丢弃用户指定的 TTS；实时严格 profile 选择 Qwen batch 时会拒绝。

实时模式也保留旧 `/audio` 的 Qwen → DeepSeek 契约；即使 WS 选了豆包/CosyVoice，旧 HTTP 路由仍不改成对应新模型或音频响应。旧入口 `python server/audio_test_server.py` 仍可独立运行。

## 原生 Opus 依赖

运行依赖固定在 `server/requirements.txt`：`aiohttp==3.14.5`；Windows AMD64 环境安装 `PyOgg==0.6.14a1`，由其 wheel 提供 `opus.dll`。项目通过 `ctypes` 调用 libopus C ABI，使用 raw Opus packet，并未采用 Ogg/WebM 容器。

当前 Windows DLL 能加载且完成编解码测试，但 `opus_get_version_string()` 返回 **`libopus unknown`**，无法据此确认 libopus 上游发行版本。本机已检查的 DLL SHA-256 为：

```text
5fa25b62f72f880acf420120d8ca32d31997620cadaab26ae798d13355012d1b
```

wheel 版本与文件哈希便于复现，不替代上游版本来源证明。若需要明确的 libopus 发行版本，可配置 `OPUS_LIBRARY` 为已核对的原生库绝对路径，并重新跑编解码测试及配置检查。加载顺序是显式 `OPUS_LIBRARY` → 系统查找 → Windows PyOgg DLL；Python 与 DLL 架构须一致。

Linux 使用系统 libopus，例如 Debian/Ubuntu：

```bash
sudo apt-get install libopus0
python3 -m venv .venv-voice
.venv-voice/bin/python -m pip install -r server/requirements.txt
.venv-voice/bin/python server/voice_server.py --list-options
```

Linux 本轮没有实际运行验证。缺失原生库时明确报错，WS 不在当前流中静默切回 PCM；调试时由客户端显式请求 `pcm_s16le`，且服务配置必须允许回退。

## PC 端互通与手动结束发言

工具输入为大于零、最长 30 秒的无压缩 PCM16、单声道 16 kHz WAV。准备文件并启动服务后：

```powershell
.\.venv-voice\Scripts\python.exe tools/voice_client.py server/data/question.wav --output server/data/reply.wav
.\.venv-voice\Scripts\python.exe tools/voice_client.py server/data/question.wav --codec pcm_s16le
.\.venv-voice\Scripts\python.exe tools/voice_client.py server/data/question.wav --cancel-after-ms 300
# 同一连接取消后继续下一轮；首个音频包到达后立即封禁旧回复
.\.venv-voice\Scripts\python.exe tools/voice_client.py server/data/question.wav server/data/next_question.wav --cancel-trigger first-audio --cancel-after-ms 0 --output server/data/multiturn.wav --report server/data/multiturn.json
```

默认连接 `ws://127.0.0.1:8000/ws/voice`，可用 `--url` 指定设备将来连接的服务地址；也支持 `--profile` / `--asr` / `--llm` / `--tts` 会话选择。工具发送 `speech.start` 并等待对应 `speech.started` 后，才按 20 ms 节奏发送音频，避免云握手期间填满上行队列；随后发送带有效尾部位置的 `audio.stream.end` 和 `speech.end`。多个 WAV 在同一连接中逐轮发送，turn/stream ID 递增。这是当前 PC 联调的明确回合边界，尚无本地 VAD 自动判停。

返回音频保存为单声道 PCM16、24 kHz WAV，默认 `server/data/voice_client_reply.wav`。工具不会实时播放，不发送 `playback.done`，不能将“已保存回复”视为“用户已听到回复”。取消测试先本地封禁旧回复标识，再发送 `response.cancel`，旧包不得进入解码输出。

多轮输出使用 `*_turn01.wav` 等文件名；JSON 默认与 `--output` 同名，也可用 `--report` 指定。报告包含模型别名、转写/回答、逐阶段时间和音频字节数，不含端点或密钥。`--cancel-turn` 指定取消第几轮（默认第一轮）；`--cancel-trigger` 可选 `response-start` 或 `first-audio`。完整结果必须有非空 ASR、非空回答、正常结束的非空音频，错误或缺少预期取消均非零退出。

向真实配置的服务提交 WAV 会调用所选云服务。当前 PC 工具串接完整 ASR → LLM → TTS 的小样本云端对话、多轮及取消恢复已通过；详情见 [验收记录](p2/p2_cloud_acceptance.md)。

## 已执行检查及后续验收

本轮在 Windows Python 3.12 的 `.venv-voice` 中复跑全部 **209项服务端测试**，通过且无跳过。检查包括旧HTTP／配置回归、各层选择与错误处理、厂商协议、SSE增量及截断、八种ASR／LLM／TTS编排组合、单轮去重、Opus首尾裁剪及本机WebSocket互通；新增录音取消覆盖握手中、流中、提交中、迟到旧包和下一轮恢复，清理停滞受5秒上限约束。既有30秒ASR节拍、突发／慢消费、待机、后台日志隔离和资源释放回归继续通过；设备侧控制器、自动缓冲、AFE及UI验证数量见本文开头。

```powershell
.\.venv-voice\Scripts\python.exe -m unittest discover -s server/tests -t server -v
```

当前本机的 `VOLCENGINE_ASR_API_KEY`、`VOLCENGINE_TTS_API_KEY` 均已正确加载，实时 profile 的配置检查和原生 Opus 加载检查通过。已完成的真实云端基础检查：

- 豆包 ASR 2.0：发送约 300 ms 的生成静音，成功建立请求并取得最终响应。这验证资源和鉴权可用，不代表真实语音识别准确率已验证。
- 火山 TTS 2.0：使用 Vivi 2.0 合成“你好，语音测试。”，成功取得约 1.58 秒的 PCM16、单声道、24 kHz 音频，保存为 `server/data/volcengine_tts_check.wav`。这验证当前音色权限及输出格式，不代表 ESP32 扬声器已播放。
- 合成后识别：将上述测试音频一次性转换为 16 kHz PCM 并提交豆包 ASR，最终识别文本为“你好，语音测试。”，与原句一致。测试使用生成语音，没有使用用户录音、LLM 或 ESP32；该检查不代替真实麦克风场景验收。

DeepSeek SSE 已在完整实时链路中通过。旧阿里云/DeepSeek HTTP 路径此次只做离线回归；可选 Qwen LLM/CosyVoice 模型和地域地址仍需替换模板占位符。

P2默认组合的小样本整链路验收已完成：Opus/PCM单轮、同连接三轮及首音频后取消恢复均通过；当时普通短句提交到首音频约2.3–2.5秒，取消确认62ms／资源释放复验78ms，恢复轮首音频约1.5秒。同一输入上行含协议头由PCM79968降至Opus10457字节，减少约86.9%，不含WS/TCP/TLS；两次转写的少量差异不能用于判断压缩准确率。这些是历史PC基线，后续安静环境手动实机对话和待机已获用户确认，首尾音、停播和资源趋势专项仍待完成。其他账号未配置的选项保持“待云验证”。

当前文本分句采用句末标点、120 字上限及结束 flush，尚无定时最大等待；后续补充分句等待和更细的服务内部延迟统计，不能把首个 LLM 增量到音频的间隔等同于纯 TTS 耗时。

第八批`17284ff146`已由用户运行，最新日志确认身份、四轮上行结束和健康／资源样本，用户反馈“一切正常”；见[AEC问题复盘](p4/p43c_aec_issue_retrospective.md)与`build/p43c_eighth_repair_hardware_observation.json`。第八批日志归档时未要求追加验证。后续用户已完成播报期屏幕基本观察：仅播报时均为静音，同时讲话转为人声、说完恢复静音，主观通过；受控声学／双讲词句保真、完整取消／长播及声学校准仍待记录，该 C 记录时 D 关闭；现已按用户决定推进 D2，不重做已跳过的 B。
