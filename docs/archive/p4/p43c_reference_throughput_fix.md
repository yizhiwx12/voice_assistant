# P4.3 C第五批：参考处理吞吐优化

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

> 历史批次记录。下文“当前／最新／尚未烧录”等安排保留当时语境；当前第八批已实机运行、用户确认“一切正常”。完整问题演变、正常运行证据和未验收边界见[AEC完整复盘](p43c_aec_issue_retrospective.md)。

更新：2026-11-11。本文保留第五批软件交付时点的修改、测试、构建身份及原报告。后续用户已烧录第五批，附件 `a8bafb17-b913-4ba8-addb-ee541acd2330` 确认AEC ELF `85b9bd3452` 已运行，仍因 `frontend_backlog` 失败：冻结AFE入口8/8、当次参考320/320、配对年龄100ms，RX有效且无溢出。C硬件未通过；[第六批小智对照与交接背压修复](p43c_xiaozhi_backpressure_fix.md)已实现，主机及默认／AEC双构建通过，新AEC ELF `df8d8151b2`，尚未烧录，新硬件结果待验；第五批设备证据见 `build/p43c_fifth_repair_hardware_observation.json`。

本批原始依据为第四批AEC ELF `6e80a67e73`：附件 `6a2348a9-210b-4b0e-bcb0-1a8932c60b69` 的AFE入口正常，首错为参考配对 `pair_age`。第五批因此减少时间线重复计算并对参考源采用局部优化。下文的软件身份、证据和检查保留第五批结果，不改写为第六批。

## 本次首因证据

- 启动确认CPU1上的capture／reference／frontend优先级为7／6／6，第四批确实生效。
- 30.697秒上报`pair_age`：队首`408640`，RX边界`411904`，年龄3264样本／204ms，配对队列10/10。`microphone_unavailable`是配对失败后主动停用AFE产生的日志。
- 首错冻结的AFE入口queue0、peak2，feed与output均408064，已完成feed最大36.268ms；RX有效、overflow0、invalid0。此次首因不能沿用上一批`frontend_backlog`。
- 参考转换cursor2567、最新2586，待转换19个10ms块。参考时线healthy1、unmapped0，但必要参考处理已经落后。
- 失败尝试`queried=0`，因为年龄检查先触发；`last_query_valid=189/320`是同一目标的上次查询，不能当作失败瞬间再次查询结果。冻结的映射包络末端408989虽超过目标末端408960，也不保证中间逐点完整，不据此绕过年龄保护。

完整提取记录为`build/p43c_fourth_repair_hardware_observation.json`。51秒后的`?`指标属于后续实时状态，不能倒推后续drop／gap是本次首因。第四批交付报告保留原时点，不覆盖成第五批结果。

## 实施内容

1. 在时间线append时缓存每块有效位置及整数边界，查询时复用。查询仍遍历固定历史元数据，但不再为所有无关块重复计算软件double乘法、ceil／floor。
2. 跨块插值先作保守范围预判，排除无关块的距离和边界计算；保留原来的精确距离及ceil／floor判定，只有实际`begin<=end`时才计算距离倒数。无关但有效的前块仍更新连接状态；无效块、缺口和身份过滤继续切断连接。
3. 增加轻量`HealthRange`供必要参考轮询使用，将完整rate／ppm诊断计算留给状态日志。映射包络仍不替代实际逐样本覆盖查询。
4. 仅`reference_resampler.cpp`、`reference_timeline.cpp`、`reference_monitor.cpp`、`playback_reference.cpp`使用`-O2 -ffp-contract=off`；其余应用保留原调试编译方式。保留IEEE检查，未启用fast-math或浮点重关联。

滤波器仍为191tap、24kHz→16kHz的2/3多相FIR；全局采样坐标、double时钟模型、相位／warm-up及约1.979ms组延迟保持。AEC调度仍7／6／6，AFE入口160ms、配对200ms和参考等待80ms均保留；引脚、云端配置和模型不变，`calibrated=0`，D插话关闭。

## 软件与构建证据

- FIR独立标量oracle：35,499个PCM样本逐个精确一致，覆盖13个reset段、11组检查；日志`build/aec-throughput-resampler-tests.log`。保留191tap、滤波系数、相位与群延迟。
- 时间线优化前生产源码保存在`build/aec-throughput-baseline/`，仅重命名类型后与新实现同编译对照；4组高变化PCM完整轨迹的PCM、mask、匹配数、完成标志及包络hash完全一致。覆盖负初始坐标、2³²双向250ppm漂移／抖动、2⁵²大坐标、跨块边界、身份／代次、缺口、warm-up、历史淘汰及未来查询。固定hash已进入常规测试：`5ebb41417ad5f83d`、`e8db635882c60779`、`f1a5e694070a7c63`、`4b3bee86ffaf5d5c`；日志`build/aec-throughput-timeline.log`、`build/aec-throughput-timeline-differential.log`。
- monitor 7组、AudioService M／MR及播放参考主机检查通过；日志`build/aec-throughput-monitor.log`、`build/aec-throughput-audio-service.log`、`build/aec-throughput-playback-reference-tests.log`。时间线、monitor及AudioService主机编译显式使用`-ffp-contract=off`。
- 最终静态核查记录`build/aec-throughput-final-static-audit.log`：4个参考源使用局部O2，其余应用仍为Og；11个音频对象无64位原子helper，RX／TX ISR保留在IRAM且无FPU指令。它不替代设备任务栈、实际墙钟耗时和资源水位。

隔离编译审查记录在`build/aec-throughput-optimization-audit.log`。原FIR常见tap路径由约14条指令变为10条，保留独立mul／add；这不是整条链路的实测加速比例。实际墙钟吞吐、同级任务竞争及栈水位仍需设备验证。

| 构建 | 应用大小 | ELF前缀 | bin SHA256 |
| --- | ---: | --- | --- |
| 默认`build/`，AEC关闭 | 2,743,904 bytes (`0x29de60`) | `83e6a22dde` | `657fa1110ecf5b13dc440c4dcea75762f58cd73cc308a23267debbc421181a38` |
| 独立`build/aec-test/`，AEC开启 | 2,744,448 bytes (`0x29e080`) | `85b9bd3452` | `86752ef2614b13327407bc6943cab0934fa81f0e9bb5430519bf2089659e42fb` |

两套应用的3 MiB分区分别剩401,824／401,280 bytes，均约13%；模型仍288,069 bytes，SHA256 `2485a4caf5754547459622a0ba126428c3dab1cef8bf57ba24e463586d2232ee`。

构建日志为`build/aec-throughput-default-build.log`、`build/aec-throughput-aec-build.log`；最终报告为`build/p43c_reference_throughput_verification.json`。主机验证音频、覆盖及生命周期约束，不能证明MCU实时吞吐已达标，也不能替代AEC声学验收。

## 第五批用户短项与后续结果

以下命令和检查表保留第五批交付时点；第五批现已烧录但短项未通过。第六批软件检查及双构建现已通过，下一次请按[第六批记录](p43c_xiaozhi_backpressure_fix.md)核对新AEC ELF `df8d8151b2`后执行。

在ESP-IDF终端分两行执行，先关闭其他占用COM7的监视器：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build/aec-test -p COM7 -b 115200 flash monitor
```

- [x] 第五批已烧录并确认ELF前缀`85b9bd3452`，调度仍为CPU1、7／6／6；只确认版本身份。
- [ ] 连接后输入一次`?`，保存起始资源及采集／参考／AFE状态。
- [ ] 安静环境完成3轮说话、`s`结束并听完回复，重点观察首次出声；结束后输入`?`，AFE active1、feed／output与pairs继续增长，RX无溢出，无`pair_age`／`frontend_backlog`／`reference_gap`。
- [ ] 一次播报中按`c`，确认取消及下一轮恢复正常；保留结束后的资源快照。
- [ ] 若失败，保存完整上电日志、首错附近的`aec_service`、`aec_reference_progress`、`frontend_failure`、`capture_failure`及随后一次`?`。不要求重做已跳过的B长时验收。

短项通过后再继续远端单讲、近端单讲、双讲和刚停播的C声学评估。
