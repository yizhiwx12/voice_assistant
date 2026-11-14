# P4.3 C 第八批：播报供给与前端协作调度修复

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

更新：2026-11-11。第八批 AEC 固件已实际运行，最新附件启动 ELF `17284ff14…` 匹配 `17284ff146`；约120秒内四轮上行正常结束，未见看门狗、首错或AEC停用，用户反馈“一切正常”。本轮基本功能和主观播报复测正常，C受控声学效果与长期稳定性仍待验收。全部问题演变和解决方案见[AEC完整复盘](p43c_aec_issue_retrospective.md)。下文“五次看门狗”保留修复前证据，不代表当前仍在发生。

## 新日志能确定什么

附件为 `e763064a-6d49-48e1-8be3-623fd9538ab9/已粘贴的文本.txt`，SHA256 `f25ae7931ecc52e745f1e07815d9ba15afa122b79ff90896336b6cfa8a1ba2a7`。硬件观测独立保存于 `build/p43c_seventh_repair_hardware_observation.json`；[第七批时钟与接收调度记录](p43c_clock_scheduling_fix.md)保留原交付结果。

| 证据 | 记录与解释 |
| --- | --- |
| 五次看门狗 | 39.391、65.083、120.972、163.564、189.318 秒，均为 `IDLE1` 未及时运行；这是 CPU1 总体调度压力的直接证据 |
| 回溯 | 包含 fetch 的 PCM 统计、feed 的 AEC FFT、队列接收及 LVGL 绘制／字形处理；任务名快照与回溯可能取样不同，不能按 `afe_fetch` 名称认定它独占五秒 |
| 前端 | `ready=1`、`aec_active=1`、`error=0`，feed／output 持续增长；这次没有前端永久失效证据 |
| 参考与 RX | `healthy=1`、`clock_errors=0`，RX valid、overflow／invalid 为 0；不能继续按旧 `reference_gap` 解释本次卡顿 |
| 下行接收 | `dispatch_work core=0`，预算等待计数持续增加；39.391 秒时 CPU0 为 `IDLE0`，核分配不均是可检查的因素 |
| 内存 | 内部可用约 59 KB、最大块 31744 bytes、`alloc_failures=0`；没有本次内存分配首因 |

当前日志没有播放 DMA 实际欠载、逐次写入间隔与听感的同步记录，也未提供配对服务端数据。因此可以修复已找到的本地调度与供给路径问题，不能宣布每次断音均已定位，亦未排除网络、TTS 或其他供给因素。

零散 `Ringbuffer of AFE is empty` 表示 fetch 当时没有取得足量结果；SDK 的 `fetch_with_delay(50ms)` 是最大等待时间，有数据时可以立即返回。它不是前端停止的证据，也不能单凭警告证明 SDK 忙等。五次看门狗的大量串口回溯可能放大卡顿，但不是已证明的最初原因。

## 第八批修复范围

1. **分担 CPU1 前端负荷。** AEC feed 保持 CPU1／优先级 6，fetch 及其 VAD／输出处理移 CPU0／优先级 6。capture 保持 CPU1／7，play/reference 保持 CPU1／6；默认非 AEC 路径保留既有策略。不给播放任务升至 7，以免参考处理重新压住 feed 或争用采集时限。
2. **为连续就绪的前端设置实际等待。** AEC feed／fetch 每累计 8 ms 保守处理墙钟或 4 次尝试后 `vTaskDelay(1)`；覆盖快速空返回，延迟放在 SDK PCM 统计、复制与输出回调完成后。等待及其计数不是 CPU 利用率，不能据此宣称算力已量化。
3. **减少补 TX 前的重复参考计算。** 保留每帧入口重 poll、每 10 ms 写片与取消检查；只有必要 AEC 目标仍在等待时，写前保留受限 catch-up。帧中使用轻量 pump，最后写片后重 poll 一次，常态重 poll 从每 20 ms 帧五次降为两次。参考按序转换，未知缺口仍失败，200 ms 配对年龄保护不变。
4. **播放任务协作让步。** AEC 播放每累计 8 ms 墙钟或两帧，在完整帧处理及回调完成、释放扬声器锁后等待 1 tick；不在两个 10 ms 写片之间增加等待。停止、错误与取消及时退出；不丢帧追赶。
5. **补充现场供给证据。** `local_frontend: schedule` 记录前端分核、`budget_attempts=4`及预算等待；`playback_flow`／`playback_service` 记录按播放作用域的解码、排队等待、队列暂空、写入耗时和写入间隔。写间隔为前次写结束至下次写开始，排除drain；这些耗时均为墙钟。队列暂空或较长写间隔是供给线索，不能直接称作 DMA 欠载计数或每个断音的根因。

不扩 DMA／播放／AFE 队列，不调整看门狗，不改变引脚、采样率、191-tap FIR、声学参数、云模型或网络保护。参考所有权仍由原播放侧串行持有，不让 fetch 直接并发访问时间线。

## 软件检查与产物

软件结果与硬件结果分开记录。第八批报告为 `build/p43c_playback_verification.json`，构建日志为 `build/aec-playback-default-build.log`／`build/aec-playback-aec-build.log`；测试日志使用同一 `build/aec-playback-` 前缀。

| 项目 | 当前状态 |
| --- | --- |
| Frontend 默认／AEC 主机回归 | 默认26／AEC35场景通过；覆盖分核、实际预算等待、SDK 快速及长空返回、完整 PCM／顺序、停止与失败清理 |
| AudioService 默认／AEC 主机回归 | 默认／AEC通过；覆盖正常重 poll 减量、必要参考追赶、70 ms 阻塞写、原 200 ms 年龄保护、FIFO／取消及锁外等待 |
| 默认 ESP-IDF 6.1／ESP32-S3 构建 | 通过；真实编译对象／链接／配置及实际产物已核对 |
| 独立 AEC 构建 | 通过；真实编译对象／链接／配置及实际产物已核对 |
| 既有控制器／时线回归 | 控制器30场景、参考时间线及clock-jitter检查通过；本轮共执行5个测试runner |
| 设备运行及卡顿复验 | 用户最新附件确认第八批身份，四轮上行正常、未见WDT／AEC停用，用户反馈“一切正常”；完整声学／专项另验 |

| 构建 | 应用大小／剩余 | ELF 前缀 | bin SHA256 |
| --- | --- | --- | --- |
| 默认 `build/`，AEC关闭 | 2,751,744 bytes／393,984 bytes | `89e67a2a8e` | `a3f89744e328a4d954c57c6fe42766f4c41bf5e9100b37c1f250e81f7637bb1a` |
| 独立 `build/aec-test/`，AEC开启 | 2,752,288 bytes／393,440 bytes | `17284ff146` | `8f594f1bda2b92d94e4d88a59520b9841f7488bdf9798fc0fa5c6bbc98adf2ab` |

两套模型镜像均288,069 bytes，SHA256 `2485a4caf5754547459622a0ba126428c3dab1cef8bf57ba24e463586d2232ee`，未改变模型或分区。软件报告 `flashed=false`、`hardware_accepted=false` 保留交付时点；随后用户烧录运行另记 `build/p43c_eighth_repair_hardware_observation.json`，不覆盖旧软件报告。本轮整理没有自动烧录。

主机检查不模拟实际 ESP-SR DSP 和双核调度；实际等待能降低持续就绪压力，但不能保证其他任务组合下 `IDLE1` 必然及时运行。`dma_underruns=unmeasured` 明确表示未测量实际DMA欠载，不能用累计submitted／completed差值推导DMA占用。构建通过也不等于看门狗消失或播放无卡顿。当前仍 `calibrated=0`，D 自动插话关闭，B 跳过项保留。

## 用户短项复验

以下保留交付时的烧录／复验步骤；用户现已运行对应新版本，不要求因此重复烧录。需要部署同一配置时，先关闭占用 COM7 的监视器，在 ESP-IDF 终端执行：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build/aec-test -p COM7 -b 115200 flash monitor
```

1. 从启动开始保存日志，核对本批 ELF `17284ff146`（启动可能仅显示前9位）、`MR` 和前端 feed core1／fetch core0；先确认待机就绪。
2. 输入 `a`，在安静环境完成三轮中文自动对话，包含一轮稍长播报；记录是否仍有卡顿及大致发生时刻。
3. 播报中按 `c`，随后重新 `a`，确认自动监听及下一轮恢复，旧声不恢复。
4. 在播报结束后输入 `?`，保存前端 schedule、playback 工作统计、dispatch、参考／RX健康、任务栈和内存；失败时保留首错及完整回溯。

- [x] 第八批主机回归及默认／AEC 双构建通过，实际产物身份和报告已填写。
- [x] 用户已运行新版本，启动ELF匹配；约120秒四轮上行结束成功，用户确认“一切正常”，本次无CPU1看门狗或AEC失效，供给／资源快照已取得。
- [x] 91.351秒播放栅栏后前端保持健康，随后第四轮正常结束，有停播后恢复证据。
- [ ] 对照精确按键序列单独验收较长播报、`c/a`恢复、主观停声尾音和旧声不恢复；不由日志中栅栏时长代替全部专项结论。
- [ ] 在健康参考前提下完成 C 的远端、近端、双讲和停播尾声声学验收。

最新附件为 `78018aa0-d596-406b-ac02-151ee0403406/已粘贴的文本.txt`，SHA256 `867ec1234c9de60bcdf67c03d855e0d618e7278bc922cfa464495325a75157ea`。末次前端ready1／AEC active1／error0，feed/output均1,830,400；RX valid1／overflow0／invalid0，参考healthy1／clock_errors0／unmapped0，配对5740次，分配失败0。仍有AFE empty等警告，输出持续前进，不写成“无任何警告”。播放最低剩余栈684 bytes保留后续观察；本次正常不等于长期或声学定量验收通过。详细作用域指标和判断边界见[完整复盘的最新证据](p43c_aec_issue_retrospective.md#13-最新正常运行证据)。

不要求重做已跳过的 B／14 分钟验收。总进度见[实施进度](../realtime_voice_progress_20261112.md)、[P4 清单](p4_local_audio_checklist.md)及[C 观测方案](p43c_aec_observation.md)。
