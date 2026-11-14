# P4.3 B收尾：按播放保存声学候选统计与任务预算

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

更新：2026-11-10。**本批代码、主机检查及ESP-IDF6.1构建通过，应用2731648字节；尚未烧录，也没有本批硬件验收。** 软件报告为`build/p43b_stats_verification.json`，构建日志为`build/p43b_stats_build.log`。应用bin SHA256为`4bb009a5892b3160c8a1f539f1584c0267099f74e0c5845bd7d0c499977311ad`，启动ELF SHA256前缀应为`4e0b0163e7`；模型未变。此前约852.625秒的[B数字观测／资源验收](p43_reference_alignment_acceptance.md#本轮b批实机记录)属于上一版固件，继续有效，不作为本批实机结果，也不要求重做整段14分钟测试。

本批解决“累计accepted增加，但最后一帧被拒绝后看不到已通过延迟”的诊断缺口，同时分开记录参考任务耗时并减少采集任务栈临时存储。AFE仍为单麦`M`，AEC、NS、AGC和自动插话关闭；云模型、网络保护、750ms预算、引脚及麦克风增益不变。

## 软件范围与边界

| 变化 | 实现和解释 |
| --- | --- |
| 按播放保留候选 | 固定保存最近4个`generation + playback_serial`作用域，带创建时`volume`标签，新编号开新记录；旧记录按容量滚动。拒绝帧不覆盖最近一次已通过候选。 |
| 聚合统计 | 保存最近通过的delay／correlation／margin／polarity、已通过delay的min／max、0～80ms共81个1ms直方图格，以及8类实际拒绝原因计数。固定聚合存储位于PSRAM，不保存录音，不增加无界历史。 |
| 任务诊断 | `reference_work`拆分pump、转换批次、数字查询和声学分析最大墙钟耗时；总耗时包含pump及调度等待，不是CPU占用或硬实时截止保证。 |
| 有界消费 | 每次转换机会最多8块，使用4ms软预算；单块被抢占后仍可能超过4ms。待处理历史通过`pending`／`budget_yields`显式记录，保持原样本顺序，不用丢块满足预算。 |
| 资源与日志 | 采集路径656字节scratch移到PSRAM；统计快照后在播放锁外输出串口，重置／drain不在长锁内输出诊断。仍须设备测量栈和实际消费余量，不能用代码优化宣称已解决历史最大耗时。 |

生产probe检查8组、monitor集成6组、AudioService行为4组、驱动100次重建／14项失败恢复、控制器30场景及本批最终固件构建通过。检查覆盖已通过候选不被拒绝覆盖、4条记录上限、身份／音量标签、直方图和拒绝计数；AudioService检查还覆盖8／8／4顺序续消费、软预算／pump计时、pending时暂停声学、旧serial／过期帧、单次最多1次分析及scratch失败／停止清理，日志为`build/p43b_stats_audio_service_reference_tests.log`。确定性RTOS／硬件替身没有运行真实并发线程，不证明真实声学、设备栈水位、实时性能或取消听感。

独立`-fstack-usage`检查见`build/p43b_stats_stack.log`：captureTask局部静态帧2048字节、playbackTask1040、pollReferenceMonitor1312、logPlaybackReference560、AcousticDelayProbe::process832、ReferenceMonitor::consume432。这只统计各函数局部帧，不含嵌套调用、RTOS／ISR，不能当成设备栈最低剩余或改善实测；新设备水位仍待本批日志。

相关只观测候选附加延迟，没有双讲检测，也不自动调相位或启用AEC。`calibrated=0`始终保留；已通过候选的分布须在固定摆位、无人近端讲话时解释，不能把单个峰值直接称为声学校准。

## 需要用户补验

保持原接线和服务端，关闭风扇，固定扬声器／麦克风距离与朝向。使用当前常用音量40先测即可；无需重新做上一版14分钟数字验收。新固件需要在ESP-IDF终端完整烧录：

```powershell
cd C:\esp-project\voice_assistant
idf.py -p COM7 -b 115200 flash monitor
```

端口不同时替换COM7，无需擦除Flash。启动先确认`ELF file SHA256`前缀为`4e0b0163e7`；本次增量构建app描述可能仍显示`Nov 10 2026 19:55:49`，时间未变不代表烧录失败，以ELF前缀／bin哈希区分新旧固件。

1. 连接就绪时输入`?`，保存`memory`、`audio_task_stacks`、`reference_work`、`reference_alignment`、`acoustic_playback_history`作为基线。
2. 输入`a`，让助手按相同文本进行两到三次较长播报。可每次问“请只朗读：春天的早晨，阳光照进窗户。小鸟在树枝上唱歌，街道上的人们开始新的一天。我们准备一杯热水，慢慢整理今天要做的事情。”；实际播报文字尽量一致。只在助手等待时提问，播报期间保持安静，不移动设备。每次播放结束后输入`?`并保存完整的`acoustic_playback`、`acoustic_hist`、`acoustic_rejections`及资源／任务行；4条记录容量有限，及时保存。
3. 再开始较长播报，正在听到声音时输入`c`。确认是否立即停止、有无旧声恢复；输入`?`，重新`a`完成一轮。说明“听感正常／异常，c发生在正在播音时，旧声有／无恢复”。软件停播栅栏数字不能替代这项陈述。
4. 最后输入`?`，提供上述日志，并说明摆位、音量和是否全程远端单讲。出现断连时另附同epoch／turn服务端首因；不以断连直接解释相关拒绝。

## 日志读法

| 输出 | 验收时观察 |
| --- | --- |
| `acoustic_playback_history` | `available=1`，`count<=4`，按新到旧输出。没有播放时count为0可解释；没有新作用域不重复开记录。 |
| `acoustic_playback` | 核对`generation`、`playback_serial`、`volume`和`has_accepted`。只有`has_accepted=1`时，`last_delay_samples`、`min_delay_samples`／`max_delay_samples`才是已通过候选；样本率16000，16样本=1ms。`last_correlation`和`last_margin`为千分数，`last_polarity`可为±1。 |
| `acoustic_hist` | `bin_ms_count=20:5,21:2`表示20ms通过5次、21ms通过2次；只输出非零格，可能分多行。比较相同摆位／音量的多轮主要分布、离散程度和极性，不直接采用全局单次min／max作为AEC范围。 |
| `acoustic_rejections` | 统计无效输入、参考不完整、麦克风／参考低能量、低相关、边界峰、歧义峰和峰支持不足。静音、尾音及低能量被拒绝可正常；若远端播报全无通过，保留日志继续分析。 |
| 计数一致性 | `saturated=0`时，直方图格数合计应等于本作用域accepted，8类拒绝计数合计等于rejected；饱和或缺日志时不能按精确总数解释。最新瞬时`acoustic_delay_probe last_accepted=0`不应清除作用域已有的通过统计。 |
| `reference_work` | 保存`total_max_us`、`pump_max_us`、`convert_batch_max_us`、`query_max_us`、`acoustic_max_us`、`max_batch`、`budget_yields`、`pending`、`stale_capture`。`max_batch<=8`；budget_yields非零表示让出软预算，本身不等于丢帧。配合参考队列drop／overwritten／healthy判断，不能只凭4ms目标宣称硬截止通过。 |
| 资源 | 对照初末内部RAM／DMA／PSRAM的free／min／largest、分配失败及capture／playback栈最低剩余字节。本批需取得真实样本，不用旧版1176／2220字节水位冒充优化后结果。 |

## 可选音量对照

当前没有串口运行时调音量命令。常用音量40的本批记录先完成；需要低／高音量对照时，在ESP-IDF终端运行`idf.py menuconfig`，进入`Voice Assistant → Speaker volume percent`，分别设20或60，保存退出，再执行：

```powershell
idf.py build
idf.py -p COM7 -b 115200 flash monitor
```

每个音量使用相同摆位与播报文字，记录`volume`标签和新的构建版本；测试后用同一流程恢复40。这个步骤会重建／烧录，不编造运行时按键；暂时只测40时，低／高音量保持待测。

## 本批状态及后续

- [x] 按4个播放作用域保留通过候选、直方图及拒绝计数，固定PSRAM存储。
- [x] 分项墙钟耗时、有限转换机会、采集scratch和锁外日志改动完成；主机检查与固件构建通过。
- [ ] 本批固件烧录与运行证据；固定摆位、常用音量重复远端单讲的已通过候选分布。
- [ ] 主观播报及特定播放中取消后旧声不恢复；新轮正常。
- [ ] 本批任务墙钟耗时、参考连续性、栈及资源样本取得并解释。
- [ ] 根据多轮已通过分布确认可用声学延迟范围；完整声学验收未通过，`calibrated=0`。

完成本批候选范围与主观取消补验后，再实施C批`MR`双通道AEC观测；回声降低、近端讲话和双讲保真属于C批验证。D批自动插话在AEC自身验收之后。英文、偶发空识别及网络停顿继续暂缓，不调云配置、增益或超时。总体见[实施进度](../realtime_voice_progress_20261112.md)及[P4.3方案](p43_aec_execution_plan.md)。
