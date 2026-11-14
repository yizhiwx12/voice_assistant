# D2 去直流后仍误插话：真实 DSP 隔离诊断

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

更新：2026-11-11。设备附件 `17d088bb-e023-440e-b7b2-53de8247dc3c`，服务端 epoch `3013152528`。用户再次明确：扬声器接线仍断开，全程无人讲话。上一批去直流与 FD 配置不能记为解决了误插话。

## 本轮确认的事实

设备 ELF 前缀为 `e50cdecb8...`，启动明确打印 `mic_dc_blocker=on coefficient=32604/32768 state=Q16`，SDK 实际流水线为 `AEC(FD_LOW_COST, NLP_VERY_AGGRESSIVE)`。虽然前缀与前批交付不同、监视器也有 ELF mismatch 警告，但这些配置和新日志说明不能把现象归为旧 SR 或去直流未启用；不能据此声称两份二进制完全相同。

重复事件日志按身份、`at_us` 和样本位置去重后，仅一组候选到确认：

| 事件 | 输出帧／窗口 | 最新滤波前输入（未与输出对齐） |
| --- | --- | --- |
| candidate，`at_us=33412022` | sample 471040；mean 0，AC RMS 65 | mic mean 250／AC 19，reference AC 2880 |
| confirmed，`at_us=33626919` | 最后帧 sample 473920，mean 1／AC RMS 43；连续 10 帧、200 ms 窗口 AC RMS **123**，门限 **80** | mic mean 272／AC 35，reference AC 178 |

末帧 AC 43 低于 80 不表示门限代码失效：判断依据是完整 200 ms 窗口，123 确实通过。中间各帧未打印，不能从两个末端值还原窗口中间的能量、来源或频谱。

确认到匹配取消 ACK 为 202.821 ms，到首上行为 630.437 ms；服务端取消 received／cleanup_complete／ack_sent 约 0 ms。turn 1 正常识别 11 字；误开的 turn 2 返回空最终，AC RMS 53、peak 903。设备两轮发送均成功，服务端本轮最大媒体落后 103／69 ms、最大回合接收间隙均 109 ms，无本轮网络积压证据。

附件没有 `pair_age`、`aec_reference_failed`、本地 fault 或 `first_failure`，因此上一轮 208 ms 配对失效并未在这个短样本复现，不能宣布长期已解决。反复出现的 AFE empty-ring 警告本身既不是前端失效，也不能单独解释误确认。Saved PC 指向 brownout ISR 是上次复位线索，本附件没有上一份日志那样的实际 BOD 报错行，不把它与后续误确认合并归因。

## 为什么这次先隔离信号来源

已核对生产 MR 交织、动态 feed/fetch 块长、固定麦克风输出和 reference 输出关闭；锁定单麦库的 fetch 是读取处理环、单位线性增益、VAD，再复制结果。生产 `afe_linear_gain=1.0`、AGC 关闭，未发现参考声道直接当结果、隐藏数倍线性增益或重复拼接 vad_cache 的错误。

AEC 会从麦克风减去估计的回声，并不保证输出幅度总小于输入。断开扬声器而数字参考继续有信号时，预测残差是待验证假设；当前 `latest mic AC=35` 与过去 200 ms `output AC=123` 未对齐，不能据此证明 AEC 放大或参考注入。SDK 也没有声明 PCM 固定算法延迟，`vad_delay_ms=128` 是判定／缓存设置，不能直接当成输入输出物理延迟。

因此本批不再抬高门限或新增未经验证的原始音量硬门，先让真实 ESP-SR 接收已知输入、给出可判因的结果。诊断完成和声学修复完成分别记录。

## 两项诊断实现

独立编译开关 `VOICE_AFE_SIGNAL_PROBE_BOOT` 默认关闭，依赖 D1／FD。诊断固件在 AudioService 的物理采集、播放和任务初始化之前，顺序创建真实 `LocalAudioFrontend` 跑五组，每组 64,000 样本／4 秒。五组走正常 MR、连续 DC blocker、feed／fetch、320 样本回调与候选观察器路径：

| case | 麦克风输入 | 数字参考输入 | 用途 |
| --- | --- | --- | --- |
| `zero` | 全 0 | 全 0 | 检查全零基线 |
| `ref_only` | 全 0 | 第 1 秒后开始有确定包络的合成信号 | 检查仅数字参考是否足以产生输出／VAD／确认 |
| `dc_noise` | DC 300＋固定种子低噪声 | 全 0 | 模拟已见偏置与低 AC 基线 |
| `dc_noise_ref` | 与上一组完全相同的输入 | 与 ref_only 相同 | 对照数字参考的影响 |
| `impulse` | sample 16000 为已知脉冲，其余 0 | 全 0 | 记录输出峰值的 ordinal 位置差，辅助检查延迟 |

这些合成信号不提交给物理扬声器、不上传云端。每组必须正常 stop／销毁后才创建下一组；资源、输入、输出数量或停止失败会返回错误并阻止普通音频启动。停止超时保留 worker 仍引用的 context，不释放活对象。五组完成后以全新 AFE 启动普通功能，避免把合成参考训练状态带入实际使用。日志中的 `would_confirm` 只是独立观察器计数，不会发送取消。`status=completed` 仅表示该组流程和统计完整，不表示输出干净或误报已修复。DC 预测统计标为 `dc_stats=expected_same_filter`，不是 feed 后回读；每组 10 秒期限从初始化前计时，但不能抢占不返回的同步 SDK 初始化。

另外，FD 前端在成功出队后记录原始麦克风、DC 后麦克风及参考的数字统计，固定 128 个 20 ms 块放在约 9 KiB PSRAM，只保存和、平方和与峰值，不保存录音。成功 feed 后才公布可用边界，失败重试不重复计入。候选／确认日志查询候选之前额外 1536 样本及完整候选窗；查询向外覆盖完整 320 样本块，缺失、过期、并发覆盖或尚未 feed 返回 `valid=0`，不等待、不冒充静音。每 20 ms 新增三路 320 样本统计，事件查询在 fetch 回调内执行；没有热路径分配或应用 mutex，实际额外调度开销仍需设备指标确认。

新 `barge_in_source` 保留事件时刻的三路 mean／RMS／AC／peak／最高帧 AC、请求／覆盖／可用范围，以及 `ordinal_mapping=1 lag_calibrated=0 waveform_aligned=0 diagnostic_only=1`。96 ms guard 是诊断观察范围，未证明覆盖全部 DSP 延迟；即使 `valid=1` 也不等于波形相位对齐。这些字段不参与打断决策，不改变 PCM、预录或确认规则。

## 软件验证与交付

软件验证已完成，报告为`build/p43d2_signal_probe_verification.json`（历史生成产物：`build/p43d2_signal_probe_verification.json`，当前目录未保留）。探针固件使用 ESP-IDF 6.1 构建，ELF SHA-256 前缀为 `3305146424`，应用为 2,779,104 bytes，3 MiB 应用分区余量 366,624 bytes；配置确认 `VOICE_AFE_SIGNAL_PROBE_BOOT=y`、FD/AEC、AC 门限 80、确认窗 200 ms。host 侧生成器、source history、前端、控制器和 AudioService 回归均通过；这些检查不执行 ESP-SR DSP，也不构成声学验收。本批新增诊断不会被写成设备误报已解决，真实 DSP 输出必须由设备运行后判读。

本轮设备只需烧录独立诊断固件，暂不重复大批真人插话测试。项目目录执行：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build/aec-signal-probe -p COM7 -b 115200 flash monitor
```

启动后保留从 `afe_signal_probe begin`、五个 `case=` 到 `afe_signal_probe end` 的日志；如果某组失败，保留第一条错误即可。优先比较 `zero` 与 `ref_only`、`dc_noise` 与 `dc_noise_ref` 的输出能量和确认次数。若合成参考单独即可产生问题，继续定位 DSP／参考残留；若不能复现，则依据正常路径的 source 窗口继续区分实际采样、电气干扰与时间线，不把未复现写成硬件已经排除。烧录前不需要关闭或修改服务端密钥；本轮未打开 COM7、未烧录。

### 2026-11-11 首次运行结果：镜像不匹配

两份启动日志都在第一个 `zero` case 之前失败：构建期模型 SHA-256 期望值为 `2485a4caf575...`，设备 `0x310000` 模型分区实际值分别为 `1ef077f8f7a8...` 和 `8790e7ddc24a...`。随后出现 `model_load error=ESP_ERR_INVALID_CRC`、`case=zero status=failed error=265` 和 `normal_start_allowed=0`。这说明设备中的模型分区没有与当前 ELF／分区表一起刷入；两次实际值不同也表明刷入时混用了旧产物或没有完整覆盖模型分区。

探针在模型校验失败时主动拒绝继续启动；`main.cpp:6` 的 `ESP_ERROR_CHECK(app.init())` 随后调用 `abort()`，因此日志中的 `rst:0xc (RTC_SW_CPU_RST)` 是软件 abort 后的重启。首行 `rst:0x3` 的 `Saved PC=rtc_brownout_isr_handler` 只保留为历史 brownout 线索，不能解释这次 CRC 重启链；本次日志没有看门狗或内存不足证据。

修复时必须从同一个 `build/aec-signal-probe` 目录完整写入 bootloader、分区表、应用和模型四个镜像，尤其确认 `srmodels.bin` 写到 `0x310000`。本次不要使用带 `--skip-flashed` 的旧刷写命令；历史输出曾把旧模型标为 `already in flash, skipping write`。刷写输出应出现 `Writing 'srmodels/srmodels.bin' at 0x00310000`；启动后应出现 `Verified ESP-SR=... sha256=2485a4...`，不再出现 `Model checksum mismatch`，并继续打印 `case=zero` 的完整统计。只写这四个地址不会擦除 NVS；不需要先执行全片擦除。

本次再次复测仍命中同一问题：设备启动 ELF 为 `01d9ff952`，模型期望 `2485a4...`，实际为 `22abe190...`，且 `case=zero` 的 `in=0 out=0` 表明 DSP 尚未开始。不要把 `01d9ff952` 当成错误重点；当前本地 `build/aec-signal-probe` 应用也可能是该构建时点，真正不一致的是模型分区。优先停止 monitor 后单独强制写 `0x310000` 模型分区；若读回 SHA 仍不是 `2485a4...`，再检查 COM7 是否为目标板及供电/闪存写入。

关联：[上一批去直流与配对重试](p43d2_dc_and_pair_retry_fix.md)、[D2 交接](p43d2_barge_in_handoff.md)、[进度](../realtime_voice_progress_20261112.md)。
