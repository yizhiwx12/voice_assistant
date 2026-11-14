# 2026-11-12：播报开头卡顿、插话漏检与时钟失效

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

## 本轮日志的直接证据

| 附件 | 证据 | 可以得出的结论 |
| --- | --- | --- |
| d7d72ace | 首答 serial=2 成功；binding_fault=0；历史 queue_dropped=9 不增；AEC 持续 active=1 | 首答绑定修复生效，未复现旧 playback_binding/pair_age |
| d7d72ace | 播报期间 candidates=0，VAD 没有 speech 转换 | 候选入口没有触发，降低候选后的 AC 门限对此无效 |
| 89b15c97 | candidates=2、confirmations=0、energy_rejections=2；200ms 窗口 AC RMS=28、25 | 两个候选被当前 80 门限拒绝；没有逐段声源标签，不能把所有候选都归为真人或回声 |
| 004fbb64 | 两轮累计 candidates=14、confirmations=4；mode=observe、interrupt=0、upload=0 | 已产生确认，但当前 D1 只观察，不执行停播和上行交接 |
| 004fbb64 | 74.689s 和 81.873s：IDLE0 watchdog；现场分别 voice_play、voice_ws；81.929s reference_gap | 存在 CPU0 实时性问题，后半段插话观察确实失效 |
| 004fbb64 | aec_clock_failure source=microphone reason=clock_jump residual_us=55981；timeline_healthy=0、last_query_valid=0；overflow=0、invalid=0、frontend_waits=0、frontend queue=0 | 参考查询失败的直接原因是麦克风时钟健康失败，不是 AFE 输入积压、pair_age 或首答绑定 |

看门狗现场栈不能证明栈上的某一个函数连续运行了 5 秒，也不能单独证明它造成麦克风时间戳迟到。日志明确证明 CPU0 空闲任务饥饿和约 56ms 的时钟残差；具体 ISR 迟到来源尚未被单独锁定。没有关闭看门狗或放宽时钟/年龄保护。

用户贴出的服务端 epoch=1211286109/turn=1 对应 d7 的首次录音：45440/16000=2.84 秒，outcome=text、8字、无削波；当前 lag=56ms、loop_lag_max=25ms，没有这次 ASR 长时间卡死的证据。它描述首轮上行，不记录播报期的插话音频，不能用于判断双讲时真人是否被 AEC/VAD 保留。

## 播报开头卡顿

d7/89 播放队列峰值只有 4/5 帧，即约 80/100ms；内容写入最大间隔为 63.68/52.01ms。空队列等待累计 15.1/11.1 秒也包含正常流式供给等待，不能把它全部算作听到的卡顿时长。当前日志明确标记 dma_underruns=unmeasured，故不能由此断言实际 DMA 欠载。

旧启动预填是取出首帧后等队列至少 4 帧，或 100ms 到期；超时时可能仅持有很少 PCM。服务器下行允许 200ms 初始突发后按 20ms 节奏供给，设备需要实际的启动水位抵御抖动。

本轮只在 AEC 路径改为：按当前 serial 已成功入队的 PCM 样本累计，水位 3840 样本/160ms，包含持有的首帧并正确计入 pre-skip/尾部裁剪；收到流结束标记可提前开始，软超时为 300ms。等待期间每 5ms 继续服务参考配对。取消或停机不提交旧首帧。启动等待受真实调度影响，300ms 不是硬实时上界。

新增按需 playback_startup 快照记录 ready、buffered_samples、target_samples、wait_us、deadline_hit、end_received；只在用户请求指标时打印，播放线程不打印这条日志。非 AEC 路径保留旧四队列帧/100ms 规则。

## 任务分配

本地当前 ESP-SR 2.6.1 单麦静态库及既有 ELF 的符号/反汇编表明：feed 同步执行 AEC，fetch 同步执行 VAD，没有创建内部 afe_se_task。旧 esp_afe_sr_1mic.ref 与当前二进制接口不同，不用于推断运行时任务。

本轮将应用 afe_fetch 从 CPU0 移到 CPU1，优先级仍为 6。CPU1 保留 capture/7、feed/6、fetch/6；CPU0 保留播放/reference/6 和下行分发/5。每个工作预算、输入队列 12 帧、采集配对队列 10 帧、200ms 年龄限制均保持原值。SDK preferred core/priority 只记录配置字段，不宣称存在库内任务。

这针对 CPU0 同时承担播放、参考转换、VAD、解码的负载。CPU1 的剩余容量仍需设备证明，不能从看门狗瞬间 CPU1 在 IDLE1 就认定它长期空闲；应检查迁移后是否出现 feed backlog、frontend_waits 或 pair_age。

## 插话模式与声学判断

当前根 sdkconfig 启用 D1，但 CONFIG_VOICE_BARGE_IN_ENABLED 未启用。b 只观察；当前镜像不能通过 i 获得 D2 停播效果。本轮没有切换固件变体或偷偷启用 D2。

候选窗的 frame/window AC 统计与实际判定音频一致；异步 mic/ref/output 快照未样本对齐，不能用于计算 AEC 衰减。source 历史也标记 lag_calibrated=0、waveform_aligned=0，不能仅由数值相除证明 VERYAGGR 压掉了真人语音。当前 min_ac_rms=80 与 NLP 档位保留；门限是否适合真人/无人播报场景需在稳定实时链路后校准。

自动插话证据日志改为按事件种类、时间戳和完整回复身份去重，避免每次状态日志重打同一份旧 energy/source。显式 ? 仍可重打最近证据。保留原有 latest snapshot 合并语义，没有新增队列、分配或回调日志。

## 主机验证与设备操作

通过主机检查：check_audio_service_reference.py（含部分首帧、160ms 样本水位、参考持续服务、300ms 超时、短流 pre-skip/裁剪、取消与停机）、check_local_frontend.py（mono/SR/FD 生命周期）、check_conversation_lifecycle.py（事件去重）、check_playback_reference.py、check_audio_driver.py。主机假实现不验证双核实时性能或真实消回声效果。

固件构建、烧录、服务启动和硬件验证由用户执行，本轮未执行。当前 build/barge-in-test/project_description.json 指向项目根 sdkconfig，继续使用同一目录：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build\barge-in-test build
idf.py -B build\barge-in-test -p COM7 -b 115200 flash monitor
```

需要启动服务时，在另一个终端运行：

```powershell
cd C:\esp-project\voice_assistant
.\.venv-voice\Scripts\python.exe server\voice_server.py --mode realtime --profile realtime_default
```

下一轮有区别的验收：启动日志 feed_core=1 fetch_core=1；两条回复开头的听感；一次 ? 快照中的 playback_startup；是否还出现 task_wdt/clock_jump/reference_gap；候选的 energy_rejections/confirmations。D1 的通过标准是候选和确认，不能用是否停播评估。不要持续刷 ?，一份快照即可保留新增证据。
