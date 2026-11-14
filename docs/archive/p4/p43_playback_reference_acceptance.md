# P4.3 A：播放参考观测实施与验收

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

更新：2026-11-10。本批代码、主机检查和 ESP-IDF 6.1 构建通过；用户已提供约495秒设备日志。**参考捕获与资源快照验收通过；主观听感／逐项取消不能仅凭串口证实。** 本批仍是播放参考观测，继续现有手动／半双工自动交互；AEC、24→16 kHz 重采样、麦克风对齐和自动插话未启用。

本文保留A批版本的实现及实机证据；下文“本批”均指A批。当前B批已补足serial、稳定静音、连续重采样／数字时线及有限相关诊断，软件／构建通过，用户约852.625秒日志验证数字参考／重采样／对齐及本次资源快照；主观听感／特定取消、受控声学范围及完整硬件验收未完成，见[B批验收](p43_reference_alignment_acceptance.md)。A批“尚未实现”的边界不代表当前B代码状态。

最新B收尾的通过候选／分布／拒绝统计、任务分项及栈优化软件／构建通过，尚未烧录／新批验收；当前操作见[B收尾统计验收](p43b_acoustic_statistics_acceptance.md)。本页A及上一版B历史证据继续保留，无需重做已通过数字观测；本批先补重复远端单讲候选范围、主观取消及任务资源，再C AEC、D插话。

P4.2 中文识别基本交互已获用户认可；英文效果、偶发短候选空识别、风扇／弱音和间歇断连继续保留为待处理项。本批不把这些问题标为修复，不修改现有 750 ms 上行保护预算。进度总览见 [实施进度](../realtime_voice_progress_20261112.md)和 [P4 清单](p4_local_audio_checklist.md)。

## 已落地的职责与路径

| 文件 | 本批职责 |
| --- | --- |
| `main/audio/max98357a_audio.*` | 按实际接受字节统计提交，注册 TX 完成回调，传入音量缩放后的 DMA 内容，记录 SD 切换、排空及 DMA 重建代次。 |
| `main/audio/playback_reference.*` | ISR 内部 RAM 固定槽、样本位置、连续性标志、任务到 PSRAM 历史的交接与诊断快照。 |
| `main/audio/audio_service.*` | 统一持有驱动，在现有扬声器互斥锁内消费参考事件，在空闲播放任务中定期消费，不新增参考任务。 |
| `main/app/conversation_controller.cpp` | 原 `?` 诊断增加参考输出，继续输出内存、上行和前端信息；不承担 PCM 处理。 |
| `main/Kconfig.projbuild`、`main/CMakeLists.txt` | 增加观测开关和生产源注册；新文件已进入实际构建。 |
| `tools/check_playback_reference.py`、相关 `tools/tests/` | 检查真实生产参考模块、驱动集成和故障退化，不模拟声学验收。 |

`CONFIG_VOICE_PLAYBACK_REFERENCE_ENABLED` 默认 `y`，依赖实时语音功能。驱动 `Config::playback_reference_enabled` 自身默认 `false`，仅实时 AudioService 根据开关启用，且仅支持当前 24 kHz 播放。启动提示音仍使用独立默认驱动配置，不开启观测。关闭此开关并重建可对照既有半双工基线。

## 数据含义与边界

1. `writeMono()` 先缩放音量，再写入相同的左右槽。`submitted_frames` 采用 `i2s_channel_write()` 实际接受的完整立体声帧数，包含错误时已接受的部分。提交成功仅代表交给驱动，不能当作已经播放。
2. ESP-IDF 6.1 的 `on_sent` 提供刚完成的 DMA 缓冲，本驱动保留 `auto_clear_after_cb=true`。回调在驱动清零／归还缓冲前复制左槽数值，并检查左右槽是否一致；任务不保留 DMA 借用指针。
3. TX 配置为 4 个描述符、每块 240 个立体声帧，24 kHz 下每块约 10 ms。`completed_frames` 包括初始空 DMA、欠载零值与静音期间的数字时钟推进；不能用它和应用提交数相等作为验收条件。
4. SD 静音时只推进完成计数与样本游标，不保存每块 PCM，下一有效块带 `gap_before`。SD 切换记录边界，下一个保存块带 `amplifier_boundary`；日志累计为 `amp_boundary`。回调读到的启用状态不能证明整块期间均处于同一电平，跨边界数据不能直接当作连续声学参考。
5. 8 个内部 RAM 槽只保存最近未消费的事件。槽满时丢弃新参考事件，递增 `queue_dropped`、锁存 `discontinuity`，下一保存块显式带缺口；正常音频播放继续，不能隐瞒缺口后宣称参考正常。
6. PSRAM 历史最多 30 块。连续保存时约为 300 ms；静音期间不物化 PCM，因此这只是启用播放期间完成 PCM 的有限滚动历史，**不是完整的连续全时线**。`history_overwrites` 是窗口滚动计数，不能与 ISR 槽溢出混淆。
7. 每次成功重建 TX DMA 增加 `dma_generation`，旧代次事件不会进入新队列。有限旧历史仍带代次保存，查询时可以筛选；不能作为新播放的连续参考。本批历史尚未绑定应用回复的 `playback_serial`，下一步对齐需补足该关系。
8. 参考的 `first_sample_24k` 为单 ISR 写入的 64 位游标，含静音期间的推进；`observed_end_us` 为 DMA 完成回调观测时间。MAX98357A 无声学回授，**DMA 完成不等于精确 DAC／扬声器出声时间**，更不代表房间回声已经消失。

正常 `drain()` 继续写入超过 DMA 容量的静音，参考也能看到这些零值；取消仍先完成本地停播与 DMA 栅栏。ISR 不进行滤波、AFE 调用、网络请求或控制器操作。

## 内存、并发与失效退化

当前目标结构每块约 512 字节：8 槽约 4 KiB 内部 RAM，30 块历史约 15 KiB PSRAM，另有对象计数器、分配器元数据及任务栈临时块。以上为结构与容量预算，**不是实机空闲内存测量**。没有增加参考工作任务，也没有把 DMA 缓冲迁往 PSRAM。

ISR 槽使用 `MALLOC_CAP_INTERNAL`，历史使用 `MALLOC_CAP_SPIRAM`。当前静态 app 的 owner／原子计数器位于内部 BSS。任务侧消费、快照和驱动生命周期由现有扬声器互斥锁串行化；队列生产／消费使用明确的发布和获取顺序。

本项目启用了 `CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND`，因此不能用 `is_always_lock_free=true` 概括平台。已核查 IDF 源和本批 ELF：32 位原子辅助函数对内部 RAM 转入 S32C1I 硬件 CAS 分支，相关函数、回调和时间戳均在 IRAM；当前内部 RAM owner／计数器不进入外部 RAM 的全局临界区分支。后续不得随意将这些对象迁往 PSRAM，也不能仅凭主机测试推断 ISR 安全。

停止以完整 channel disable／delete 成功为资源边界：单独 `disable` 不作为回调退出和释放栅栏。停止失败时保留仍可能被驱动引用的存储，不强制释放；确认旧 channel 删除后才重置代次或释放参考内存。

| 情况 | 当前行为与验收含义 |
| --- | --- |
| 内部事件槽分配失败或回调注册失败 | 关闭参考观测并报告，原播放路径继续。不能视为参考验收通过。 |
| PSRAM 历史分配失败 | 保留内部事件计数与消费，`history_available=0`、`history_unavailable` 增加；没有可用 PCM 历史，不能作为连续参考或后续 AEC 的合格输入。 |
| 非整帧／部分写入、异常块大小、槽溢出 | 记录对应诊断，标记缺口／异常；不自动放宽上行预算，不据此开启 AEC。 |
| 正常 SD 静音、切换或历史窗口滚动 | 明确计数及边界，不能单看 `gap_before` 就判为 Wi-Fi 故障。 |

## 已完成的软件检查

| 检查 | 结果及证据 |
| --- | --- |
| 生产参考模块 | 10 组场景通过：值拷贝、静音／SD、左右不一致／异常块、槽满缺口、代次、历史与分配退化；`build/p43_playback_reference_tests.log`。 |
| 生产 MAX98357A 驱动 | 100 次静音 DMA 重建、GPIO 所有权、14 项注入失败及无 SD 场景通过；并通过 observer 联合检查，覆盖音量后样本、尾块／drain、部分超时、取消代次、注册／分配失败、停止失败存储保留、零音量与欠载；`build/p43_audio_driver_tests.log`。 |
| 生产控制器 | 30 个生命周期场景通过，含既有自动模式、上行竞态、旧播放期 AFE 恢复栅栏与结果保留；`build/p43_controller_tests.log`。 |
| 固件 | ESP-IDF 6.1／ESP32-S3 构建成功；`build/p43_build_final.log`。 |

主机检查不验证真实ISR延迟、DMA时钟、声学播放或可用内存趋势。A批软件实施时未修改服务端处理逻辑或凭证，未执行云调用、服务重启和烧录；之后用户已提供下方设备证据。A批产物与当前B批产物分别记录，完整进度以[实施进度](../realtime_voice_progress_20261112.md)为准。

## A批实机操作步骤

保持当前接线、云模型、增益与音量。先关闭风扇、固定摆位，用中文进行对照。本批无需新增接线、申请密钥或调整 TTS 音色；保持现有服务端运行即可，不必为此替换服务配置。用户已提供包含多代播放参考、手动／自动前端、Wi-Fi 与内存诊断的日志；下面步骤用于复测或补充主观听感证据。

1. 在 ESP-IDF 终端进入项目，使用当前串口完整烧录并打开监视器：

   ```powershell
   cd C:\esp-project\voice_assistant
   idf.py -p COM7 -b 115200 flash monitor
   ```

   COM7 与设备实际端口不同时替换。无需擦除 Flash。完整 `flash` 会按项目清单部署应用、分区和配套模型；不在本批单独更换模型。

2. 连接就绪、尚未录音时输入 `?`，保存 `memory`、`local_frontend` 和 `playback_reference` 行作为基线；然后输入 `a` 开启现有半双工自动模式。分别说一句中文短句和较长句子，各等回复播放完后再输入 `?`。确认文字／播报正常、没有新增卡顿；播放期间仍然不会自动插话。
3. 让助手生成较长回复，播放中输入 `c` 取消，随后输入 `?`；确认声音停止、旧声音不会恢复。按现有操作重新输入 `a`，再说一轮并等待播完。比较取消前后参考 `generation`，保存新一轮诊断；不要求每次正常播完都增加代次。
4. 输入 `m` 切回手动；输入 `r` 后说完整中文句子，输入 `s` 结束，等待识别和播报。确认原始手动路径仍可工作。本批不以“播放中说话能自动打断”为验收要求。
5. 最后输入 `?`，保存本次基线、两次正常播放、取消和手动一轮的配套日志。提供 `playback_reference`、`memory`、前端诊断及出现的失败行；若遇间歇断连，同时保存同 epoch／turn 的服务端日志，单独归类，不把它直接归因于参考观测。

## 日志判断与通过条件

| 字段 | 应如何判断 |
| --- | --- |
| `generation` | TX 重建后增加；重建后计数从本代次重新开始，历史可能仍包含带旧代次的有限块。 |
| `submitted_frames` | 播放提交增加，静音 drain 也包含在内；不与文本字数或音频包数直接比较。 |
| `completed_frames`、`blocks`、`muted_blocks` | 包含持续运行的 TX 数字时钟和静音段，待机增长并非残留语音继续播放。 |
| `content`、`silence` | 有声播放应观察到内容块，drain／欠载可观察到零值；这不是 ASR 语音／静音分类。 |
| `amp_boundary` | SD 切换后的边界块计数，可以非零；对应 PCM 不能当作整块声学有效性证明。 |
| `partial_bytes`、`invalid`、`stereo_mismatch` | 正常播放期望为 0；非零时保留整段日志，不手工归零掩盖。`partial_bytes` 是异常写接受的字节数累计，并非丢失字节总量。 |
| `queue_dropped`、`discontinuity` | 正常测试期望为 0；非零表示本代次参考不连续，应排查事件消费或驱动写入。SD 正常边界／静音历史空档另有标记，不等同于此异常锁存。 |
| `queue`、`history`、`history_overwrites` | 队列有界，历史不超过 30；窗口持续滚动可使 overwrites 增长，不能将它当作槽溢出。 |
| `history_available`、`history_unavailable` | 正常应为 1／0；缺失 PSRAM 历史虽不阻断半双工，也不能通过参考验收。 |
| `memory` | 对照internal free／min／largest、DMA、PSRAM和`alloc_failures`。本批已有约495秒快照且分配失败为0，仍不能凭一次快照宣称长期无泄漏。 |

本轮已满足的通过条件：设备多代 DMA 有非零内容块，历史可用且不超过30块，队列与格式异常计数为零，分配失败为零，并有约495秒运行后的内存快照。未由附件直接证明的项目：用户主观听感、某次 `c` 是否发生在正在播音时及确切声学停播时刻。若这些仍需确认，按上方步骤复测并提供简短结果；参考日志不失效时保持半双工，仍不自动开启AEC或插话。

A到B的历史依赖是连续有状态24→16kHz重采样、有效静音／SD边界、播放身份及麦克风时序对齐；B现已实现，软件／构建及本轮数字观测／资源快照实机通过。下一步按[B批验收](p43_reference_alignment_acceptance.md)补受控声学范围、主观听感及特定取消。A批静音空档、SD边界和回调时间不能直接作为已校准AEC输入；C AEC及D插话仍待后续独立验收。

## 本轮实机日志记录

附件日志 `C:\Users\yizhi\.codex\attachments\98032216-753f-4423-849f-3e5be4eb3a26\已粘贴的文本.txt` 覆盖上电至约 495 秒，设备已加载 8 MB PSRAM、ESP-SR 模型校验通过，Wi-Fi 最终连接并建立 WebSocket。启动时曾一次临时关联拒绝，随后在重试后连接成功；这不是本轮持续断连证据。

参考观测在多个 DMA 代次有明确 PCM 活动。例如 generation 2 有 `submitted_frames=121081`、`content=500`，generation 3 有 `submitted_frames=313171`、`content=1300`，generation 9 有 `submitted_frames=304291`、`content=1264`。generation 2–9 的 `history_available=1`、`history_unavailable=0`；日志中 `partial_bytes`、`stereo_mismatch`、`invalid`、`queue_dropped`、`queue` 和 `discontinuity` 均为 0。`history=30` 是容量上限，随后 `history_overwrites` 持续增长属于窗口滚动；`muted_blocks` 与 `amp_boundary=1` 是静音／SD 状态观察，不是队列故障。

末尾 `?` 对应内存约为 `internal_free=74255`、`internal_largest=31744`、`dma_free=66467`、`dma_largest=31744`、`psram_free=7671832`、`psram_largest=7602176`、`alloc_failures=0`。该日志中也有多轮自动 VAD／上行和最终回到 `manual_raw` 模式的记录。约 8 分钟观察未见分配失败或参考队列异常；不据此推断所有更长时段均无泄漏。

**验收结论：P4.3 A的设备端参考捕获、历史有界性、代次变化及本次资源快照通过。** 串口日志证明数据进入了数字I2S DMA参考路径，不证明扬声器的主观听感或精确声学时序。附件没有逐项注明用户是否听清每轮播报，也无法仅凭代次变化区分所有DMA重建是取消还是新流准备；若主观播报／取消未实际核对，仍需在后续回归中确认。A批之后的B已补足有效静音／SD边界和`playback_serial`映射，并完成重采样／数字时线及相关诊断软件，设备验证见[B批验收](p43_reference_alignment_acceptance.md)；本轮没有测试或启用AEC、自动插话。
