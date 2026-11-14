# P4.3 C第四批：播报期间AFE调度与首因快照

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

> 历史批次记录。下文“当前／最新／尚未烧录”等安排保留当时语境；当前第八批已实机运行、用户确认“一切正常”。完整问题演变、正常运行证据和未验收边界见[AEC完整复盘](p43c_aec_issue_retrospective.md)。

更新：2026-11-11。本记录保留第四批交付时的实现及构建事实：第三批AEC镜像`e5d17b6925`已烧录，两份日志开始播报时AFE入口积压；第四批将feed/fetch与reference调整为同优先级并冻结UART前首因，主机与双构建通过，当时尚未烧录。其后用户已烧录第四批AEC ELF `6e80a67e73`，仍首先出现`pair_age`204ms：AFE入口queue0／peak2、RX健康、参考待转换19块。失败尝试`queried=0`，189/320属于上次查询；未据映射包络端点绕过逐点覆盖或年龄保护。最新软件及烧录短项请转到[第五批参考吞吐优化](p43c_reference_throughput_fix.md)，新AEC ELF `85b9bd3452`，尚未烧录。C未通过、`calibrated=0`，D关闭；本页不将旧报告改写为新批结果。

## 本轮设备证据

两份附件的应用ELF前缀均为`e5d17b692…`，启动日志均为CPU1上的capture／reference／frontend优先级7／6／5，确认第三批调度已实际运行。

| 附件ID | 首次错误 | 首次失败附近已保存的采集／配对证据 | 之后输入`?`的实时前端指标 |
| --- | --- | --- | --- |
| `267b62f1-0e2d-474a-aa74-861103baac0b` | 116.191秒`frontend_backlog`／`ESP_ERR_TIMEOUT`，随后`frontend_pair`／`ESP_FAIL` | RX时线有效、overflow0、invalid0；read最大32.107ms、间隙最大2.581ms；队首100ms，目标参考320/320，pending0，时线healthy1／unmapped0 | 127.023秒：入口queue8／peak8，feed1776128／output1775616，feed最大184.044ms、fetch最大154.237ms |
| `759d4fa5-7468-4844-b253-4f05e5e46446` | 32.369秒`frontend_backlog`／`ESP_ERR_TIMEOUT`，随后`frontend_pair`／`ESP_FAIL` | RX时线有效、overflow0、invalid0；read最大31.923ms、间隙最大3.135ms；队首112ms，目标参考320/320，pending0，时线healthy1／unmapped0 | 39.786秒：入口queue8／peak8，feed435200／output434688，feed最大240.623ms、fetch最大191.552ms |

这里首先耗尽的是项目的8×20ms AFE入口队列，不是ESP-SR内部50帧环，也不是200ms麦克风配对队列。`frontend_pair`是前端入队失败继续上报的第二条错误；旧实现还把实际入队超时改写成了`ESP_FAIL`。

两份`?`快照分别晚于首错误约11秒和7秒。旧代码先输出UART，已进入的feed/fetch仍可能继续完成；因此184／240ms是之后读取的调用墙钟最大值，不能称为首故障时已冻结的DSP耗时或CPU占用。后来的参考drop、gap及discontinuity同样不能倒推为此次首因。新证据没有显示首故障时RX溢出或目标参考缺口，也不足以单独判定总CPU负荷。

## 本批实现

- **让必要参考与AFE共享处理时间。** 仅AEC配置在CPU1采用capture7、播放／reference6、AFE feed6及fetch6。采集仍可抢占；参考与DSP处于同一优先级，利用已启用的FreeRTOS抢占、时间片与1kHz tick轮转。默认AEC关闭的`M`路径保留原有核心选择及优先级。
- **保留所有有界保护。** AFE入口仍8帧／160ms，麦克风配对仍10帧／200ms、等待参考80ms；参考单次8块／软4ms及写入前有界追赶不放宽。没有跳帧、删除旧音频追赶或把未知参考当作有效零。
- **冻结前端首次失败。** `LocalAudioFrontend::FailureSnapshot`在首次失败的UART输出前保存错误、`at_us`、入口queue／peak、feed／output样本计数及已完成调用的feed／fetch墙钟最大值，发布后不被次生错误覆盖。这是失败附近的原子计数观测，字段不是RTOS调度跟踪，也不包含尚未完成的调用最终耗时。
- **延后打印详细证据。** AudioService保存前端首次快照，随后在既有延后日志中输出`frontend_failure`，与`capture_failure`和配对查询证据一起核对。详细输出不占用采集回调或播放锁内的实时处理。
- **保留入队错误原码。** `enqueuePaired()`失败时把实际返回值传递给AEC失败记录，入口满将保留`ESP_ERR_TIMEOUT`，不再统一改为`ESP_FAIL`。

源码中播放与参考由同一个`voice_play`任务串行处理。每10ms PCM写入前后会处理参考与配对，TX初始DMA缓冲或连续下行使该任务可能持续就绪；原优先级6严格高于AFE的5，单独`taskYIELD()`不能让较低优先级任务得到时间片。把AFE提升到同级修正了这一调度关系。当前锁定库的1mic `afe_feed`同步调用AEC处理，调整外层feed任务确实影响实际DSP调度。

**时间片公平不等于总CPU预算足够。** 真实非零参考加入后的AEC计算、重采样、Opus解码、Wi-Fi及显示负载仍需实机验证。主机替身不模拟RTOS优先级或真实双核抢占，不能用主机通过宣称播报期间已稳定。

## 软件与构建记录

本批LocalAudioFrontend单麦26／MR28场景及AudioService单麦／MR检查通过。覆盖AEC／M任务创建策略、首错快照保留和重启清除、次生错误不覆盖、错误原码保留及现有未知缺口／容量保护；临时移除失败后fetch排空行为的负例被新回归正确拒绝。快照在UART之前发布的次序经源码审查确认，未由这些测试直接断言。前端执行摘要在`build/aec-fairness-frontend-tests.log`，不是原始测试stdout。这些检查只证明受控替身下的功能约束，不模拟RTOS优先级。

| 构建 | 应用大小 | 3 MiB分区剩余 | ELF前缀 | bin SHA256 |
| --- | --- | --- | --- | --- |
| 默认`build/`，AEC关闭 | 2,743,536 bytes (`0x29dcf0`) | 402,192 bytes（13%） | `8781583d44` | `bdf56e534ba69100e8356b4635f7a518f310ae94f23894a354c773175a5391d7` |
| 独立`build/aec-test/`，AEC开启 | 2,744,064 bytes (`0x29df00`) | 401,664 bytes（13%） | `6e80a67e73` | `54f16136bc1276974f37b6d8bdbc6cc0eb6dce1dff9376d882671751ec54aa72` |

ESP-IDF6.1双配置构建通过，两个变更生产源的编译条目与更新对象均已核对。模型仍为288,069 bytes，SHA256 `2485a4caf5754547459622a0ba126428c3dab1cef8bf57ba24e463586d2232ee`。报告为`build/p43c_frontend_fairness_verification.json`；构建日志为`build/aec-fairness-default-build.log`、`build/aec-fairness-aec-build.log`。旧报告及前三批设备失败事实保持原记录，不把历史产物改为本批结果。

静态审计记录在`build/aec-fairness-stack-audit.log`：已检查的7个音频对象无64位原子helper引用，RX／TX ISR及所需32位helper在IRAM。编译器给出的直接函数栈帧不是调用链总栈上界，不能替代设备水位。首次快照由各项原子观测组成；若另一个首错发布者尚未发布ready，延后日志中的`frontend_failure`可能缺失，该行按可取得证据输出，不阻塞实时任务等待。

## 第四批交付时的短项复验（历史）

以下保留第四批当时交付的步骤；现在请按[第五批短项](p43c_reference_throughput_fix.md)核对最新身份并烧录。第四批设备结果另记`build/p43c_fourth_repair_hardware_observation.json`。

在ESP-IDF终端关闭占用COM7的其他监视器，分两行执行：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build/aec-test -p COM7 -b 115200 flash monitor
```

- [x] 用户日志已核对第四批AEC ELF前缀`6e80a67e73`；`audio_scheduling`显示CPU1上capture7／reference6／frontend6。
- [ ] 安静环境冷启动，连接后尚未录音时输入`?`；保存AEC active1、RX有效／overflow0、配对持续增加及内存／任务栈基线。
- [ ] 完成3轮说话、`s`结束并听完回复；重点观察首次出声，播报结束再输入`?`，比较AFE queue／feed／output和配对计数，确认没有`frontend_backlog`、`pair_age`或`reference_gap`。
- [ ] 播报中按一次`c`，确认停声且旧内容不恢复，下一轮仍可识别播报；保存前后资源快照。
- [ ] 若失败，保留完整上电日志、第一条错误以及其后的`frontend_failure`、`capture_failure`、`aec_service`、`aec_reference_progress`和一次`?`。将冻结快照与后续实时指标分别判断，不重复已有长时B验收。

以上短项通过后，才继续C的远端单讲、近端单讲、双讲和刚停播声学评估。B跳过项仍未验收，D插话继续等待C结果。
