# P4.3 C 第七批：时钟异常确认与下行解码调度修复

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

> 历史批次记录。下文“当前／最新／尚未烧录”等安排保留当时语境；当前第八批已实机运行、用户确认“一切正常”。完整问题演变、正常运行证据和未验收边界见[AEC完整复盘](p43c_aec_issue_retrospective.md)。

更新：2026-11-11。本文保留第七批时钟与下行调度的软件交付、测试和产物身份。后续附件已含core0接收／新时钟诊断，AEC与时线健康，但CPU1五次空闲任务看门狗，用户反馈播报卡顿；无启动ELF，不能断言已完整核对 `4557d8e17b`。当前推进[第八批播报供给与前端协作修复](p43c_playback_stutter_fix.md)，主机检查及双构建通过，C仍未验收。

本批不改变接线、采样率、云端模型、凭证、网络预算或 AEC 声学配置。默认 AEC 关闭，C 仍 `calibrated=0`，D 自动插话关闭；用户已跳过的 B 收尾补验继续留档。

## 后续设备反馈

附件 `e763064a-6d49-48e1-8be3-623fd9538ab9` 在39.391／65.083／120.972／163.564／189.318秒记录五次 `IDLE1` 看门狗。回溯跨AFE输出统计、AEC FFT、队列等待与LVGL；前端 `ready=1/error=0`，参考 `healthy=1/clock_errors=0`，RX无溢出、分配失败0。第七新诊断已出现，但附件无启动ELF，完整身份未核对。此为播报卡顿问题，不能继续把它等同于第六批前端永久失效；缺少逐断音DMA与配对服务端证据。独立记录为 `build/p43c_seventh_repair_hardware_observation.json`；原第七软件报告不改写。最新修复及短项见[第八批记录](p43c_playback_stutter_fix.md)。

## 第六批设备实际发生了什么

附件为 `04eb7e5d-0df5-4665-9b9f-9d3b3a239682/已粘贴的文本.txt`，SHA256 为 `bfaa1bf3af0f91be059432f9d264c5073610f430e9ec48e044587eaed68f1846`。启动 ELF 前缀 `df8d8151b…` 与第六批构建 `df8d8151b2` 匹配，因此不能再归因于刷入旧版本。独立硬件记录为 `build/p43c_sixth_repair_hardware_observation.json`。

| 时间／冻结证据 | 记录 | 能支持的结论 |
| --- | --- | --- |
| 47.698 秒 | `IDLE1` 看门狗；CPU1 当前任务 `voice_rx`，栈含 `VoiceClient::dispatch → onBinary → feedPlayback → Opus decode` | 下行处理期间 CPU1 空闲任务未获足够运行机会 |
| 85.197 秒 | 再次 `IDLE1` 看门狗；任务名快照为 `afe_fetch`，解码栈仍含上述 Opus 调用链 | 任务名与回溯的取样时刻可能不同，不能据此说 AFE fetch 调用了 Opus |
| 85.366 秒 | 首次 `reference_gap`，`timeline_healthy=0` | 数字时间线已失效，自动前端必须停止 |
| AFE 入口 | queue `0`，peak `2`，`frontend_waits=0` | 本次没有入口满队证据，也没有实际走到第六批的 busy 重试恢复 |
| 配对／参考 | 头部年龄 `120 ms`，pending `0`，converted/latest `8058/8058` | 不是本次已知的 200 ms 配对超龄或待转换参考积压 |
| RX | valid `1`，overflow `0`，invalid `0` | 本次冻结窗口没有麦克风 DMA 丢失证据 |

`valid_samples=0` 是不健康时间线拒绝查询后的结果，不能单凭这个值推断 PCM 全部丢失。健康包络覆盖目标范围也不能替代逐样本有效性查询。附件未显示网络或内存分配首因。

第六批的软件交付报告 `build/p43c_backpressure_verification.json` 保留当时的 `flashed=false`；后续实际运行结果独立保存，不覆盖历史报告。原[第六批记录](p43c_xiaozhi_backpressure_fix.md)保留其交接修复和小智代码对照。

## 本次定位与修复

### 时钟模型区分单次回调延迟和持续频偏

RX／TX 使用的是 ISR 回调执行时刻，并非硬件锁存的 DMA 完成时刻。旧实现把单个约 1 秒窗口的估计频偏超出 ±2000 ppm 立即当成永久失效。生产时间线代码的主机复现表明：仅在一个窗口边界增加 3 ms 回调延迟、保持样本计数连续，也能使旧模型失败；这证明代码存在独立可复现的缺陷，尚不能证明本次设备时钟失败必然由看门狗造成。

第七批保留原 ±2000 ppm 范围，但异常窗口不更新采样率模型；先记录可疑方向，再建立独立新窗口。连续两个独立窗口同方向超限才确认 `rate_out_of_range`。正常窗口清除嫌疑，反方向异常改记新方向，避免一次延迟产生相邻负／正假超限。

这个规则不是“任意抖动下两秒必失败”的保证。持续频偏与毫秒级回调延迟同时存在时，确认时刻可能改变；下方约两秒结果仅对应所列测试轨迹。

原先的 50 ms 残差突跳、非单调时间／样本计数、RX overflow／invalid 和配对映射保护继续明确失败；未知参考不能补成有效零数据。正常路径仍采用原每块最多 25 µs 相位校正，四个历史高变化 PCM 哈希保持一致。

### 下行处理让出实际调度时间

启用 AEC 时，`voice_rx` 固定 CPU0、优先级 5、栈 32768 bytes，与 CPU1 的 capture／reference／AFE 分开。PSRAM 栈失败后的内部栈回退保持相同 CPU0 绑定。默认非 AEC 接收任务仍不绑定核心。

两种构建都在累计处理达到 8 ms 墙钟时间或 8 条消息后执行 `vTaskDelay(1)`。等待发生在回调返回、消息缓冲释放后，音频／流锁之外；错误和停止路径及时退出。FIFO 顺序及原队列背压保持，未以 `taskYIELD()` 替代实际阻塞，也未丢弃旧帧追赶。

`rx_handler_*`／`rx_batch_*` 和 `dispatch_work` 的处理时间为墙钟时间，包含回调内等待，不是 Opus 纯计算时间或 CPU 占用率。启动 `dispatch_scheduling` 记录 core、priority、预算及 delay；`?` 触发汇总诊断，逐包不增加互斥锁或日志。

### 冻结第一个时钟失败原因

`ReferenceTimeline::firstClockFailure()` 保留首次来源、原因、样本边界、时间、残差、估计 ppm、窗口和坏窗口数。AudioService 由串行参考所有者写入不可变快照，以 release／acquire 发布给采集侧；工作任务退出后才重置。首个 AEC 失败没有时钟故障时，快照保持 `valid=0`，不会被后来错误补写。

延后输出 `aec_clock_failure valid=... source=... reason=... residual_us=... observed_ppm=... window_us=... bad_windows=...`；`?` 的 `reference_alignment` 日志附带 `clock_suspect_windows`（内部指标名为 `clock_rate_suspect_windows`）。这样下次可区分频偏、时间突跳、非单调和 RX 异常，避免继续用通用 `reference_gap` 猜测首因。实时热路径不增加 64 位原子 helper、锁等待或串口输出。

## 软件验证与产物

| 检查 | 结果及边界 |
| --- | --- |
| `tools/check_reference_timeline.py` | 通过；保留四个历史高变化 PCM 哈希 |
| `tools/check_reference_clock_jitter.py` | 21 种健康轨迹、12 种持续频偏轨迹通过；健康轨迹覆盖启动／窗口边界／重复 3 ms 延迟、±250 ppm 和 64 位样本基值，从 400 ms 后每 100 ms 检查完整 320 样本、mask 和非零 PCM |
| 持续频偏保护 | 所测 RX／TX ±2500／3000／5000 ppm（含微秒抖动）在 2010～2032 ms 拒绝；不是任意毫秒抖动下的延迟上界 |
| 生产 VoiceClient 主机检查 | 默认／AEC 各 12 场景通过；含 CPU 绑定及回退、FIFO、时间／消息预算、释放后等待、停止／失败和清理 |
| AudioService 主机检查 | 默认／AEC 通过；含首次时钟快照发布／保留、RX overflow 原因、重启重置和 `valid=0` 不回填 |
| 既有集成回归 | monitor 7 组、控制器会话 30 场景通过 |
| ESP-IDF 6.1／ESP32-S3 | 默认／独立 AEC 两种构建通过；软件报告见 `build/p43c_clock_scheduling_verification.json` |

另以 `build/aec-mixed-drift-probe.cpp`／`.log` 运行 60 秒临时混合轨迹：真实 ±2500／5000 ppm，叠加窗口端点交替 0／+3 ms 或 +4 ms 延迟，16 条均在 60 秒内被现有保护拒绝；最慢约 37 秒才因 `mapping_phase` 失败。故障前各次查询均完整、非零 PCM，无坏样本。该实验说明混合毫秒抖动会显著延后确认；不能据此宣称声学对齐精度或统一两秒检测上界。

| 构建 | 应用大小 | 3 MiB app 分区剩余 | ELF 前缀 | bin SHA256 |
| --- | ---: | ---: | --- | --- |
| 默认 `build/`，AEC关闭 | 2,747,584 bytes | 398,144 bytes | `9dcb7704dd` | `302c84c549f0ae79c6708f0f25b0ea82883d98ae8dc0771b265e094b1591829b` |
| 独立 `build/aec-test/`，AEC开启 | 2,748,112 bytes | 397,616 bytes | `4557d8e17b` | `4e47e272ce9912690d3cfa4521de7de1213addf21919a5340755f2a33f4343fc` |

主机替身验证逻辑、顺序及保护语义，不模拟真实 ESP-SR DSP、ESP32 双核 CPU 分配或声学效果。构建通过尚不证明设备看门狗已消失、实际吞吐合格或 AEC 已验收。

## 用户手动烧录与短项复验

先关闭占用 COM7 的串口监视器，在 ESP-IDF 终端分两行执行：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build/aec-test -p COM7 -b 115200 flash monitor
```

1. 核对启动 ELF `4557d8e17b`（日志可能仅显示前 9 位），AEC 为 `MR`，`dispatch_scheduling` 为 core `0`、priority `5`、预算 `8000 us / 8 messages`。上电先待机，确认本地前端保持就绪。
2. 输入 `a`，在安静环境完成三轮中文自动对话，每轮等待正常播报结束。
3. 一轮播报中按 `c`，然后重新输入 `a`，确认监听与下一轮对话恢复，旧声不恢复。
4. 再进行一轮稍长播报，结束后输入 `?`，保存从启动开始的日志。
5. 检查没有 `IDLE1` 看门狗／`reference_gap`，时间线健康、RX overflow／invalid 为 0，并记录 `dispatch_work`、预算等待计数、AFE／配对队列、任务栈及内部 RAM／DMA／PSRAM。发生异常时保留首个失败和 `aec_clock_failure`，后续 `?` 不替代冻结首因。

- [x] 第六批烧录身份和实际失败已独立记录；没有把软件交付报告改写为设备通过。
- [x] 第七批代码、主机检查及默认／AEC 双构建通过。
- [ ] 第七批完整启动身份核对；后续附件无ELF，虽含新诊断，不能勾选完整身份。
- [ ] 第七批启动／播报短项全部通过；后续前端／时线健康但五次CPU1看门狗及播报卡顿，不能记通过。当前复验按[第八批短项](p43c_playback_stutter_fix.md#用户短项复验)。
- [ ] 参考健康前提下完成 C 的远端单讲、近端单讲、双讲及停播尾声声学验收。

不要求重复已跳过的 B／14 分钟验收。短项通过后再评价 C 声学效果，D 自动插话仍在 C 验收之后。总进度见[实施进度](../realtime_voice_progress_20261112.md)、[P4 清单](p4_local_audio_checklist.md)与[C 观测方案](p43c_aec_observation.md)。
