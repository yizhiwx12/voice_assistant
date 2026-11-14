# 历史归档

返回[文档索引](../README.md)。本目录保存原始阶段记录及整理前全文，内容按当时批次解读；最新状态见[当前进度](../realtime_voice_progress.md)，实际运行请用[操作指南](../operations.md)。

旧命令、构建目录、ELF 身份、测试数量和“待验／通过”只描述对应批次。部分记录包含此前未通过的修复，不能覆盖为最新结果；D3／D4／D5 是当时的修复文件编号，不是新的必经阶段。原文件名保留，Markdown 引用已按新位置修正。

## 按问题查找

| 要找什么 | 优先阅读 |
| --- | --- |
| 正常监听被短声触发 | [空闲起音过滤](p4/p4_idle_onset_filter_20261112.md) |
| 一播报就误插话／瞬态误确认 | [误插话修复](p4/p43d2_false_barge_in_fix.md) · [D2 交接与后续持续性修正](p4/p43d2_barge_in_handoff.md) |
| 插话观察不可用 | [D1 状态争锁](p4/p43d1_controller_busy_fix.md) · [空闲 VAD／AFE 任务](p4/p43d4_idle_vad_20261112.md) |
| 播放卡顿、参考供给与调度 | [AEC 问题复盘](p4/p43c_aec_issue_retrospective.md) · [小智背压对照](p4/p43c_xiaozhi_backpressure_fix.md) |
| 开机 AEC 配对失败／QIO 与 DIO | [启动与 Flash 模式](p4/p43d5_boot_pairing_flash_mode_20261112.md) |
| 空识别、录音断连、Pong 超时 | [空识别诊断](p4/p4_empty_asr_result.md) · [P3 断连记录](p3/p3_recording_disconnect_issues.md) |

## 整理前全文快照

- [原实施进度全文](realtime_voice_progress_20261112.md)
- [原实施方案全文](realtime_voice_plan_20261112.md)

## 完整阶段目录

### P2：云端小样本验收

- [P2 默认组合云端小样本验收](p2/p2_cloud_acceptance.md)

### P3：设备录放与网络

- [P3 设备实时链路操作与验收](p3/p3_device_acceptance.md)
- [P3 问题与解决方法汇总](p3/p3_issue_summary.md)
- [P3 连续录音断连问题与处理方案](p3/p3_recording_disconnect_issues.md)

### P4：前端、自动端点与空识别

- [P4 本地语音前端实施与验收清单](p4/p4_local_audio_checklist.md)
- [P4.1 本地语音前端部署与真机验收](p4/p4_local_audio_acceptance.md)
- [P4 进度、问题与解决方法](p4/p4_issue_summary.md)
- [P4.1 内部 RAM 不足：修复与验收](p4/p4_memory_budget.md)
- [P4.1 AFE 缓冲耗尽与服务端事件循环停顿](p4/p4_frontend_ringbuffer_and_server_stall.md)
- [P4.1 录音无文字与空识别提示隐藏](p4/p4_empty_asr_result.md)
- [2026-11-12：自动监听的短促噪声起轮保护](p4/p4_idle_onset_filter_20261112.md)

### P4.3 A／B：参考、时线和声学统计

- [P4.3 播放参考、AEC 与自动插话执行方案](p4/p43_aec_execution_plan.md)
- [P4.3 A：播放参考观测实施与验收](p4/p43_playback_reference_acceptance.md)
- [P4.3 B：播放参考重采样、时序对齐与延迟观测](p4/p43_reference_alignment_acceptance.md)
- [P4.3 B收尾：按播放保存声学候选统计与任务预算](p4/p43b_acoustic_statistics_acceptance.md)

### P4.3 C：AEC、调度与播放

- [P4.3 C：AEC 测试问题、解决方案与实机结果完整记录](p4/p43c_aec_issue_retrospective.md)
- [P4.3 C阶段：可选AEC观测与验收](p4/p43c_aec_observation.md)
- [P4.3 C：启动采集与播报切换修复](p4/p43c_startup_and_playback_fixes.md)
- [P4.3 C：配对调度修复与连续采集诊断](p4/p43c_pairing_and_capture_diagnostics.md)
- [P4.3 C：音频任务调度与参考服务优先级修复](p4/p43c_audio_scheduling_fix.md)
- [P4.3 C第四批：播报期间AFE调度与首因快照](p4/p43c_frontend_fairness_fix.md)
- [P4.3 C第五批：参考处理吞吐优化](p4/p43c_reference_throughput_fix.md)
- [P4.3 C 第六批：小智实现对照与 AFE 交接背压修复](p4/p43c_xiaozhi_backpressure_fix.md)
- [P4.3 C 第七批：时钟异常确认与下行解码调度修复](p4/p43c_clock_scheduling_fix.md)
- [P4.3 C 第八批：播报供给与前端协作调度修复](p4/p43c_playback_stutter_fix.md)
- [P4.3 C：播报期间的人声／静音显示](p4/p43c_playback_observation_ui.md)

### P4.3 D1：候选观察

- [P4.3 D1：播报期间的打断候选观察](p4/p43d1_barge_in_observation.md)
- [P4.3 D1：普通状态锁争用导致插话观察不可用](p4/p43d1_controller_busy_fix.md)
- [2026-11-12：首答绑定失败与小智音频实现对照](p4/p43d1_xiaozhi_binding_review_20261112.md)

### P4.3 D2 及后续修正：交接、误触发和启动

- [P4.3 D2：自动停播、取消与讲话交接](p4/p43d2_barge_in_handoff.md)
- [D2 插话取消确认超时修复](p4/p43d2_cancel_timeout_fix.md)
- [D2 下行发送节奏与取消确认诊断](p4/p43d2_downlink_ack_fix.md)
- [D2 安静播报误打断：证据与候选窗口修复](p4/p43d2_false_barge_in_fix.md)
- [D2 播报起始误插话：全双工 AFE 配置修正](p4/p43d2_full_duplex_afe_fix.md)
- [D2 断开扬声器仍误插话与配对超龄修正](p4/p43d2_dc_and_pair_retry_fix.md)
- [D2 去直流后仍误插话：真实 DSP 隔离诊断](p4/p43d2_signal_isolation_diagnostics.md)
- [2026-11-12：播报开头卡顿、插话漏检与时钟失效](p4/p43d3_playback_bargein_20261112.md)
- [2026-11-12：播报改善后，插话观察仍失效](p4/p43d4_idle_vad_20261112.md)
- [2026-11-12：偶发开机 AEC 配对失败与 QIO/DIO](p4/p43d5_boot_pairing_flash_mode_20261112.md)

## 历史生成产物的可用性

整理前已有部分链接指向当前目录未保留的 `build/` 报告或日志。这些生成产物不是源文档；本文没有重新生成，也不以缺失文件证明历史验证结果。对应位置已保留原报告路径并明确标注，避免形成无法打开的链接。归档中其余 `build/` 路径仍按项目根目录解释。

- `build/barge-in-test/log/idf_py_stdout_output_37124`
- `build/p43d1_busy_verification.json`
- `build/p43d1_first_hardware_observation.json`
- `build/p43d1_verification.json`
- `build/p43d2_acgate_verification.json`
- `build/p43d2_cancel_verification.json`
- `build/p43d2_dc_pair_verification.json`
- `build/p43d2_downlink_verification.json`
- `build/p43d2_energy_verification.json`
- `build/p43d2_fd_verification.json`
- `build/p43d2_signal_probe_verification.json`
- `build/p43d2_verification.json`
