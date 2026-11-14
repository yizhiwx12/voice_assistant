# P4.3 C：音频任务调度与参考服务优先级修复

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

> 历史批次记录。下文“当前／最新／尚未烧录”等安排保留当时语境；当前第八批已实机运行、用户确认“一切正常”。完整问题演变、正常运行证据和未验收边界见[AEC完整复盘](p43c_aec_issue_retrospective.md)。

更新：2026-11-11。**本文第三批AEC镜像`e5d17b6925`已由用户烧录，两份新日志仍在开始播报时首先出现`frontend_backlog`。** 首次失败附近RX有效且overflow0，目标参考320/320、pending0；后续第四批同级调度和首因快照见[当前记录](p43c_frontend_fairness_fix.md)。以下保留第三批交付时的软件／构建事实；其中“尚未烧录”及原短项表示当时状态，不代表当前安排。C硬件仍未通过，不能把第三批主机或构建通过记为播报故障已经解决。

## 新设备证据

三份附件均显示`4c6ec7c8d…`，不是刷错版本：

| 附件ID前缀 | 首次故障 | 新诊断 |
| --- | --- | --- |
| `c98afb5d` | 开机`capture_timeline` | 第4次read进入未返回，首溢出为`phase=read core=0`，阶段已持续95.694ms；read最终最大158.020ms，此前回调最大86µs。 |
| `bf3472c1` | 开机`capture_timeline` | 第3次read进入未返回，首溢出为`phase=read core=0`，阶段已持续95.619ms；read最终最大173.362ms，此前回调最大73µs。 |
| `028c4b8f` | 41.222秒`pair_age` | 开机正常，先有600次已知静音配对；首次故障时RX有效、overflow0，队首年龄208ms、队列10/10、参考pending1。 |

前两份RX ISR持续产生完成事件并发生溢出，因此已排除“首次read没有调用”与“raw回调耗时过长”这两个解释。RX使用IDF默认6个DMA描述符和5个待处理队列槽，256样本／16kHz每槽16ms，待处理队列约80ms；不是TX参考观察器的8槽。read墙钟耗时包含等待和被抢占时间，尚无任务调度跟踪证明唯一阻塞来源。

两次启动故障与Wi-Fi认证／关联重试重叠，实际消费者均在CPU0。另在本批之前的ELF中确认：RMS调用的`std::sqrt(double)`在舍入路径读取FCR，FCR属于ESP32-S3 CP0/FPU；FreeRTOS协处理器异常会固定任务到当前核心。因此源码使用double仍不能保证任务始终可迁移，显式核心安排比依赖第一次浮点／PIE调用时机更可控。Wi-Fi不是已定案的唯一根因。

第三份是独立的参考消费问题。启动known-mute配对绕过真实时线查询；失败前1808次中1685次known-mute，绑定后约123帧成功，真实参考查询／播报负载加入后超龄。上一查询仅196/320有效，但旧快照没有保存对应头帧坐标，不能直接宣称超龄帧也恰为196/320；本批补齐对应关系。末次`unmapped=5`是在首故障约14秒后观测，不能用来证明首因就是映射孔洞。

## 本批实现

- **仅AEC使用明确的CPU1调度。** 采集优先级7，播放／参考服务6，AFE feed和fetch为5；任务栈位置与容量不变。采集可抢占参考处理，必要参考处理优先于普通DSP／上行任务。默认AEC关闭的M路径保持原有无指定核心及6／5／5优先级。
- **必要参考先于可选诊断。** AEC已有实际配对覆盖查询，停止在相同消费者重复做B阶段数字探测和81个延迟候选的声学搜索，也不再向不会消费的诊断采集队列写入。M／B观察路径保留原探测。AEC中的`probe_frames=0`或声学attempt不增加不再表示AEC没有工作，应看`pairs`、完整覆盖和AFE feed／output。
- **阻塞TX写入前有界追赶。** 成熟目标不完整且参考仍pending时，在下一次写入前最多提供4次总服务机会／软16ms切片；单次仍限8块／软4ms，按序消费真实参考。每轮检查运行状态、serial和错误，写入前再次检查取消；达到切片上限就返回，不能无限占用处理器。排空播放时也使用该服务。
- **冻结参考处理进度。** 首因保留最近查询的目标坐标、有效样本范围及缺失前缀／后缀／内部、转换cursor、最新history序号／待转换块数、有效映射范围和时线健康／unmapped。超龄检查不覆盖前一次查询证据。范围只是有效源点的包络，不能代替逐样本完整性查询；字段经播放侧原子发布，详细日志继续在采集任务与播放锁外输出。

没有扩大RX／参考／AFE队列或放宽200ms年龄保护，没有跳过麦克风帧、丢旧参考追赶或将未知参考补零。引脚、云服务、模型不变；AEC仍未校准，附加延迟0ms，D插话未开启。

## 软件与构建证据

本批检查通过：AudioService M 9／MR 25组；LocalAudioFrontend M 25／MR 27场景；时线检查及monitor 7组集成检查。新增动态回归由同一墙钟驱动TX 10ms、RX 16ms和采集帧20ms，并在参考处理期间持续生产数据、耗尽单次预算；进入注入的70ms写入停顿前，必须先按序配好成熟目标。删除写入前追赶调用的临时生产副本在该断言失败，当前代码通过。该注入不是实机写入耗时结论。

另验证任务创建的核心／优先级、失败清理、参考尾部缺失mask、64位参考前沿，以及超龄后首查询证据不被覆盖。主机检查不模拟真实双核抢占，不能保证CPU1负载、AFE队列与栈水位已合格。

| 构建 | 应用大小 | 3 MiB分区剩余 | ELF前缀 |
| --- | ---: | ---: | --- |
| 默认`build/`，AEC关闭 | 2,743,312 bytes (`0x29dc10`) | 402,416 bytes（13%） | `f834a6872c` |
| 独立`build/aec-test/`，AEC开启 | 2,743,824 bytes (`0x29de10`) | 401,904 bytes（13%） | `e5d17b6925` |

```text
默认bin SHA256  9b73aa2cd2f90e84af5f9bb155233ab1fb02ac6ab267242874bb09b2ae976ff7
AEC bin SHA256  ac4ad250a22b8a007da87bace9ecbb1893d5fafccbcb834e195aae2230119ee7
```

两套ESP-IDF 6.1构建均通过，已核对7个相关生产源的编译条目和新对象。模型仍为288,069 bytes，SHA256 `2485a4caf5754547459622a0ba126428c3dab1cef8bf57ba24e463586d2232ee`。报告`build/p43c_scheduling_verification.json`；构建日志`build/aec-scheduling-default-build.log`、`build/aec-scheduling-aec-build.log`；测试日志为`build/aec-scheduling-*-tests.log`；反例为`build/aec-live-clock-red/before-catchup-result.log`。不覆盖前两批交付报告及硬件失败事实。

## 第三批交付时的用户短项（历史）

以下命令和ELF身份仅保留第三批交付记录；当前请按[第四批短项](p43c_frontend_fairness_fix.md)核对镜像。第三批当时要求在ESP-IDF终端分两行执行，先关闭占用COM7的其他监视器：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build/aec-test -p COM7 -b 115200 flash monitor
```

- [ ] 确认ELF前缀`e5d17b6925`；启动`audio_scheduling`应显示capture/reference/frontend均为core1，优先级7／6／5。
- [ ] 冷启动3次，先不录音／播报；连接后输入`?`，采集实际`core=1`、RX有效且overflow0，AEC active1、pairs增加。
- [ ] 安静环境连续3轮`r/s`并听完回复，再测试一次播报中`c`及下一轮恢复。比较前后AFE queue／feed／output、配对queue／pairs、任务栈及内存快照；不出现`pair_age`、`reference_gap`或前端积压失败。
- [ ] 若失败，保留完整上电日志、首错误及之后一次`?`，尤其`audio_scheduling`、`capture_service scope=first_overflow`、`aec_service`、`aec_reference_progress`和`capture_failure`。无需重做已跳过的B长时验收。

第三批设备结果现已确认未通过：两份新日志首先AFE入口积压。上列未勾选项保留交付时的验收要求，当前实施和复验以[第四批记录](p43c_frontend_fairness_fix.md)为准；声学校准与D插话继续等待。
