# P4.3 C：配对调度修复与连续采集诊断

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

> 历史批次记录。下文“当前／最新／尚未烧录”等安排保留当时语境；当前第八批已实机运行、用户确认“一切正常”。完整问题演变、正常运行证据和未验收边界见[AEC完整复盘](p43c_aec_issue_retrospective.md)。

更新：2026-11-11。本文保留第二批软件交付记录。**用户已烧录本批ELF `4c6ec7c8da`，三份新日志仍未通过：两次启动RX溢出，一次播报时配对超龄。** 最新第三批调度修复／构建和短项复验见[后续记录](p43c_audio_scheduling_fix.md)。本批主机通过不等于硬件通过，历史软件报告保留原交付时的未烧录状态。

## 修改内容

1. **按目标参考是否完整配对。** 最新TX尾部仍在转换时，已完整覆盖的旧麦克风帧可以按序feed；明确已知静音的帧同样可消费。目标不完整且历史仍待转换时等待，追完后仍缺则报`reference_gap`。保留80ms参考等待、200ms年龄保护、10帧容量和每次最多4帧，不丢旧帧追赶、不用零填未知参考。
2. **记录持续采集进度。** 新增首次read返回时间、调用／完成次数、最近返回样本范围、读耗时／两次读取之间的最大间隔、处理阶段、实际核心及raw回调耗时。采集在read、convert、callback、queue和stopped之间标记阶段，区分“已进入read”与“read已完成”。
3. **首个RX溢出冻结现场。** ISR保留当时的消费者进度；只做一次原子快照，无日志、分配或锁等待。与消费者发布冲突时标记`coherent=0`，不等待被抢占的任务。任务侧读取新的服务进度也只尝试一次；原有物理DMA快照与永久失效保护保持不变。
4. **首个AEC故障保留证据。** 冻结最老待配对样本／年龄、当次DMA末端、参考覆盖、pending、队列、参考服务间隔和配对耗时。使用播放侧发布的原子摘要，采集故障不能无锁读取播放临时PCM或时线。详细日志延后到播放任务或`?`、在播放锁外只输出一次；采集路径先停用前端并保留简短原因，避免新增大量UART输出阻塞采集。后续错误不覆盖首因。

没有改变引脚、云服务、模型、音频任务绑核或DMA容量。默认AEC关闭；本批独立AEC使用MR、附加延迟0ms，仍为`calibrated=0`，D插话未启用。

## 软件证据

新增的“目标完整、较新尾部仍pending”测试在修改前失败、修改后通过。AudioService生产代码检查通过：M 9组、MR/AEC 23组，覆盖持续新增尾部、已知静音、真实缺口、等待／超龄、失败清理、64位快照、发布冲突、采集阶段和首因冻结／单次延后输出。

麦克风生产驱动12组检查通过，包括170ms驱动read、250ms读取间隔、200ms回调、read未返回／回调中首溢出、发布被ISR抢占、并发快照与64位边界。前端M 25／MR 27场景、monitor 7组、重采样10组、时线、播放参考及驱动100次重建／14项注入失败检查通过。主机替身不证明真实双核调度、声学效果或资源余量。

| 构建 | 应用大小 | 3 MiB分区剩余 | ELF前缀 |
| --- | ---: | ---: | --- |
| 默认`build/`，AEC关闭 | 2,740,544 bytes (`0x29d140`) | 405,184 bytes（13%） | `96f00af1e5` |
| 独立`build/aec-test/`，AEC开启 | 2,741,072 bytes (`0x29d350`) | 404,656 bytes（13%） | `4c6ec7c8da` |

```text
默认bin SHA256  adf8fe004d2b7e8dc970f9c887f772dee9a05d41388079f3b9a4b69ceaf36854
AEC bin SHA256  3952dfe744656227080f28a05881f8075a5a0356beeafb06fde8b183d436dbe7
```

ESP-IDF 6.1两套构建均成功，已核对7个相关生产源的编译条目和对象更新时间。模型仍为288,069 bytes，SHA256 `2485a4caf5754547459622a0ba126428c3dab1cef8bf57ba24e463586d2232ee`。证据为`build/p43c_pair_service_verification.json`，构建日志`build/aec-pair-service-default-build.log`、`build/aec-pair-service-aec-build.log`，AudioService回归日志`build/aec-pair-service-tests.log`。上一批报告保留，不覆盖历史烧录与失败事实。

## 当时的短项复验清单（本批实机未通过）

以下保留当时的镜像身份与验收期望；当前应按[第三批记录](p43c_audio_scheduling_fix.md)烧录和复验。

关闭占用串口的其他监视器，在ESP-IDF终端分两行执行：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build/aec-test -p COM7 -b 115200 flash monitor
```

若实际端口不同，替换COM7。以ELF前缀`4c6ec7c8da`确认新版本；不要使用增量构建保留的编译时间判断。

- [ ] 冷启动3次，先不录音／播放；连接就绪后输入`?`。期望RX `valid=1 overflow=0`、AEC `active=1 error=0`、配对持续增加。
- [ ] 安静环境连续3轮`r/s`，听完回复；另在播报时`c`取消，再开始下一轮。期望参考与配对保持正常，不出现`pair_age`／`reference_gap`。
- [ ] 若失败，保留上电至首次错误及其后一次`?`的完整日志，尤其`capture_startup`、`capture_service scope=first_overflow`、`aec_service`和`capture_failure`。无需反复录音或重做已跳过的B长时测试。

`capture_service coherent=0`表示该次诊断快照与发布重叠、数据缺席，不是另一个RX故障。`phase=read`且调用数大于完成数表示驱动调用尚未返回；`callback`／`convert`／`queue`及phase年龄用于定位消费者停在哪一段。服务和配对字段是有时间标记的最近尝试，不能把它们当作所有任务在同一时刻的全局快照。

本批C设备结果已由用户日志确认未通过，具体新证据与后续调度修复见[第三批记录](p43c_audio_scheduling_fix.md)。
