# P4.3 C 第六批：小智实现对照与 AFE 交接背压修复

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

> 历史批次记录。下文“当前／最新／尚未烧录”等安排保留当时语境；当前第八批已实机运行、用户确认“一切正常”。完整问题演变、正常运行证据和未验收边界见[AEC完整复盘](p43c_aec_issue_retrospective.md)。

更新：2026-11-11。本文保留第六批小智对照与交接背压修复历史。第六批 `df8d8151b2`已实际运行仍看门狗／时线失效，硬件结果独立留档，未验证满队恢复；第七时钟／接收调度软件通过，后续新诊断及健康AEC已见但无启动ELF，仍播报卡顿。当前见[第八批供给／协作修复](p43c_playback_stutter_fix.md)，软件检查及双构建通过。

## 本次故障能确定什么

首错发生在启动约 30.908 秒，顺序为 `frontend_backlog / ESP_ERR_TIMEOUT`，随后 `frontend_pair / ESP_ERR_TIMEOUT`。这次是已经配好参考的音频无法交给 AFE 入口，自动前端因此停止；后续原始手动路径仍可录音和上传。

| 首错证据 | 数值 | 含义 |
| --- | --- | --- |
| 冻结 AFE 入口队列 | `8/8` | 零等待入队遇到满队列 |
| 冻结 feed/output | 均为 `411136` samples | 已完成 feed 统计与 output 相等，不排除在途 feed；不能据此证明 DSP 无阻塞或归因为 fetch 持续积压 |
| 失败配对头/RX 边界 | `414400 / 416000` | 头部年龄 `1600` samples，即 `100 ms`，尚未越过 200 ms 保护 |
| 当次查询 | `queried=1, valid_samples=320` | 本次目标的参考逐样本完整；缺前缀、缺后缀、内部缺口均为 0 |
| 参考转换 | `2610 / 2611`，pending `1` | 当时还有新参考块待处理，但不能据此否定已完整覆盖的当前目标 |
| RX 冻结状态 | valid `1`，overflow `0`，invalid `0` | 没有麦克风 DMA 丢失证据 |
| 已完成 feed/fetch 最大墙钟耗时 | `42.203 / 68.107 ms` | 包括调度等待，不能直接当成纯 DSP CPU 时间 |

第五批设备提取记录为 `build/p43c_fifth_repair_hardware_observation.json`。41 秒、68 秒、88 秒等后续 `?` 是故障后的实时状态。它们不能代替首错快照，也不能把停止后的代次变化解释成首因。该附件没有同时出现网络断连或内存分配失败首因；问题定位在本地音频交接。

第五批代码在配对成功后先从配对队列移除头部，再以零等待方式提交给 AFE。AFE 入口一旦暂时满，`enqueuePaired()` 立即冻结失败并停止前端。该处理把一次可重试的交接拥塞直接变成不可恢复故障。第六批已将这次入口暂满改为非致命 busy，并保留原队首供后续重试。日志足以证明存在这个问题，但尚不能区分它背后的全部调度抖动来源，也不能证明 DSP 平均算力不足。

## 小智是怎样实现的

本次从官方仓库读取并固定到提交 [`a73e9a7e266e3196d1368ba46c30bee0d81f0a64`](https://github.com/78/xiaozhi-esp32/commit/a73e9a7e266e3196d1368ba46c30bee0d81f0a64)，提交日期为 2026-11-04。读取的相关文件及来源元数据存放在 `build/xiaozhi-reference-review/`。本地 `C:\esp-project\xiaozhi-esp32-2.5.2` 也核对了相关音频实现，但目录名本身不能证明它是未经修改的官方发布包；引用以固定提交的官方文件为准。

| 项目 | 小智官方固定提交 | 本项目第五批 |
| --- | --- | --- |
| AFE | `AFE_TYPE_FD`，`AEC_MODE_FD_LOW_COST`，NLP `VERYAGGR` | `AFE_TYPE_SR`，`AEC_MODE_SR_LOW_COST`，保持现有 AEC 观察配置 |
| AEC运行条件 | 创建后先disable；运行时按唤醒检测，或device AEC开启且voice processing激活的状态启用 | 显式开启C选项后持续观察AEC，承担连续采集／配对与前端负荷 |
| 采集和 feed | processor 启用时，输入任务 core 0、优先级 8；每次读取 10 ms，直接拼成 AFE chunk 并 feed | core 1、优先级 7 采集；参考生产和 AFE feed/fetch 都在 core 1、优先级 6；先完成参考配对，再交接给独立 feed 任务 |
| fetch | 独立任务优先级 3，创建时未指定 core；`fetch_with_delay(..., portMAX_DELAY)` | 独立任务，50 ms 有限等待；保留停止和首错诊断 |
| 上传交接 | fetch 输出仅进入编码队列；编码及网络在后续任务执行 | AFE 输出与上行编码发送已分离，但配对到 feed 的入口交接仍有本次满队即失败问题 |
| R 参考来源 | 按板型决定；支持硬件回采的 codec 将参考与麦克风作为输入通道；Box Lite 另有软件缓存 | 独立 ICS-43434 RX 16 kHz 与 MAX98357A TX 24 kHz，根据实际 DMA 完成边界做软件重采样和时钟映射 |
| ESP-SR | manifest 固定 `2.4.7` | `2.6.1` |
| 编译 | 默认 `CONFIG_COMPILER_OPTIMIZATION_SIZE=y`；未见音频文件单独 O2/O3 特例 | 全局调试优化，四个参考处理源另用 `-O2 -ffp-contract=off` |

官方对应源码：

- [音频输入、任务与编码队列交接](https://github.com/78/xiaozhi-esp32/blob/a73e9a7e266e3196d1368ba46c30bee0d81f0a64/main/audio/audio_service.cc)：任务创建在 125–170 行；10 ms 读取及直接 feed 在 308–314 行；编码队列防止阻塞 fetch 的说明在 584–593 行。
- [共享 AFE、feed 与 fetch](https://github.com/78/xiaozhi-esp32/blob/a73e9a7e266e3196d1368ba46c30bee0d81f0a64/main/audio/engines/afe_audio_engine.cc)：FD 配置在 126–185 行；fetch 任务在 228–235 行；feed 在 258–276 行；fetch 和控制串行化在 385–449 行。它没有人为缩小默认 AFE 内部 ring。
- [Box 硬件输入参考与共时钟配置](https://github.com/78/xiaozhi-esp32/blob/a73e9a7e266e3196d1368ba46c30bee0d81f0a64/main/audio/codecs/box_audio_codec.cc)：共用 I2S duplex 且要求输入输出同采样率；输入参考通过 codec 的通道 mask 读取。
- [Box Lite 软件参考](https://github.com/78/xiaozhi-esp32/blob/a73e9a7e266e3196d1368ba46c30bee0d81f0a64/main/boards/espressif/esp32-s3-box-lite/box_audio_codec_lite.cc)：同采样率 duplex；Read 顺序读取软件参考缓存，缺数据时填 0；Write 在 codec write 后加入缓存。
- [普通 I2S 麦克风/扬声器 codec](https://github.com/78/xiaozhi-esp32/blob/a73e9a7e266e3196d1368ba46c30bee0d81f0a64/main/audio/codecs/no_audio_codec.cc) 与[基础 codec 声明](https://github.com/78/xiaozhi-esp32/blob/a73e9a7e266e3196d1368ba46c30bee0d81f0a64/main/audio/audio_codec.h)：默认没有输入参考通道；不能由“小智支持 AEC”推断相似的分立 I2S 硬件已具备有效 R。
- [依赖版本](https://github.com/78/xiaozhi-esp32/blob/a73e9a7e266e3196d1368ba46c30bee0d81f0a64/main/idf_component.yml)、[设备 AEC 板型限制](https://github.com/78/xiaozhi-esp32/blob/a73e9a7e266e3196d1368ba46c30bee0d81f0a64/main/Kconfig.projbuild)、[编译默认配置](https://github.com/78/xiaozhi-esp32/blob/a73e9a7e266e3196d1368ba46c30bee0d81f0a64/sdkconfig.defaults)。

本地 2.5.2 的相关结构也为共享 FD AFE、直接 feed、独立 fetch，Box Lite 同样采用软件参考缓存。官方固定提交相对本地目录还有代码重排、唤醒提示与部分控制流程更新，因此行号不可混用。

## 本次借鉴及不直接照搬的部分

采用小智所强调的原则：采集、前端处理与网络相互隔离；实时线程交接不能因下游暂时阻塞而长时间等待。但其编码队列“丢最旧帧”位于上传边界，不能复制到本项目 AEC 输入配对边界。丢掉某个麦克风或参考块、把未知参考补成 0，都会损坏连续时间关系。

小智 Box Lite 的同率共时钟软件缓存也不能直接替换本项目 16 kHz/24 kHz 两路独立 I2S 的 DMA 完成映射。其源码存在软件参考特例不代表已经证明本项目硬件能按同一方式正确对齐。

小智的AEC并非在所有状态始终开启：`afe_audio_engine.cc` 的205–208行在创建后先停用；394–400行根据唤醒检测，或 `device_aec_enabled && voice_processing` 状态切换。本项目C是连续AEC观察负荷，不能把小智某个状态的负载当作本项目吞吐证明。

FD AEC 与更强 NLP 会改变声学效果，且两边 ESP-SR 版本不同。本次不把 SR 改 FD、不调整 NLP，也不声称 FD 会消除入口拥塞。小智的优先级和 core 分配对应不同工作负载，本次不照抄优先级数字。

## 第六批修复范围

1. 新增配对入队的可重试接口 `tryEnqueuePaired()`：入口暂满返回 busy（`ESP_ERR_NOT_FINISHED`），不立即冻结 `frontend_backlog`。参数、生命周期或前端异常仍按其实际错误处理。
2. 配对头部只有在 AFE 入口接收成功后才移除。busy 时保留同一头部、同一顺序和完整样本，返回本次服务机会，等待后续调度重试。实时线程不长等，不额外拷贝成无界缓存，不丢弃头部追赶。
3. 原有 200 ms 配对年龄保护与固定队列容量保留；如果拥塞持续到超限，仍明确失败。此次是修复瞬态交接语义，不保证能够承受持续平均超载。
4. feed/fetch 的任务栈扫描改为每 50 个处理块一次，减少每块诊断扫描开销。正常音频处理顺序与首错快照继续保留。
5. 保持 capture/reference/frontend 调度 7/6/6、AFE 入口 8×20 ms、配对 10×20 ms、参考等待 80 ms。保持全部引脚、采样率、191-tap FIR、参考缺口保护及云端配置。

默认 AEC 关闭构建继续使用原 M/manual 行为。C 声学硬件仍未验收，`calibrated=0`，D 插话保持关闭。

## 软件检查与构建结果

- Frontend M26／MR29 场景通过。新增暂满重试覆盖20次重试以及恢复后的5120个麦克风／参考样本精确检查，确保 busy 不停用前端、不吞掉输入、不重复提交。Frontend 原始终端输出未另存日志，数量及结果保存在汇总报告中。
- 生产 AudioService M／MR 回归通过：暂时背压保留原头，恢复后非零麦克风／参考样本与顺序精确一致；原3200样本边界仍允许，3201样本仍按超龄保护失败；未知参考、资源及生命周期错误继续失败。日志 `build/aec-backpressure-audio-service-tests.log`。
- 默认和独立 AEC 的 ESP-IDF 6.1／ESP32-S3 构建均通过。核实本次 `audio_service.cpp`、`local_audio_frontend.cpp` 的实际编译来源和新对象，继续使用 `-Og`，未引入外部64位原子 helper。第五批四个参考处理源的局部优化保持。

| 构建 | 应用大小 | ELF 前缀 | bin SHA256 |
| --- | ---: | --- | --- |
| 默认 `build/`，AEC关闭 | 2,744,176 bytes | `ea23570eac` | `3fdcfff615a25987e0d72af6d502663ec97ea5ffefe9e9ae523e661ea6b16197` |
| 独立 `build/aec-test/`，AEC开启 | 2,744,736 bytes | `df8d8151b2` | `d1465f2adc32f3fbf4e2820606a0eb65b24840e965ae976fa2e1149efa93ae90` |

两套历史应用在3 MiB分区中分别剩401,552／400,992 bytes，均约13%。模型288,069 bytes，SHA256 `2485a4caf5754547459622a0ba126428c3dab1cef8bf57ba24e463586d2232ee`。当时构建日志`build/aec-backpressure-default-build.log`和`build/aec-backpressure-aec-build.log`；软件报告`build/p43c_backpressure_verification.json`的`flashed=false`／`hardware_accepted=false`保留交付时点。后来实际烧录结果另存第六硬件报告；本次未自动烧录。当前候选见[第八批记录](p43c_playback_stutter_fix.md)，软件／双构建及产物身份已核对，尚未烧录。

## 手动烧录与短项复验

以下保留第六批当时的短项清单，未勾选项不代表已执行。当前独立目录命令仍适用，应按[第八批](p43c_playback_stutter_fix.md#用户短项复验)已构建的新身份 `17284ff146` 核对烧录启动，不把旧 `df8d8151b2` 当当前候选。先关闭占用COM7的监视器，在ESP-IDF终端分两行执行：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build/aec-test -p COM7 -b 115200 flash monitor
```

- [x] 第六批代码、Frontend M26／MR29、AudioService M／MR与默认／AEC双构建通过。
- [x] 用户已烧录第六批AEC ELF `df8d8151b2`，启动前9位 `df8d8151b…`匹配；身份确认不等于硬件通过。仍CPU1看门狗／时线失效，见[第七批证据](p43c_clock_scheduling_fix.md)。
- [ ] 启动待机后输入 `a`，安静环境连续完成三轮自动监听对话；等待助手播报结束再开始下一句。确认自动检测、识别和播报均恢复，未出现本地前端异常。
- [ ] 一轮播报中按 `c` 取消，随后重新输入 `a`，确认自动监听及下一轮对话恢复，旧声不恢复。
- [ ] 最后一轮播报结束后输入一次 `?`，保留 `local_frontend`、`aec_pairing`、参考／RX健康、任务栈与内存状态；失败时保留从启动到首错的完整日志。

`aec_pairing` 新增的 `frontend_waits` 是入口暂满后等待后续服务机会的重试次数，可以大于0。它不是故障次数；需要结合 active／error、队列能否回落及后续 pairs、feed、output 是否继续增长判断。首错另保留冻结 `aec_handoff_failure frontend_waits`。若拥塞持续到原年龄保护边界，仍应明确失败，不能仅因重试未立即报错就宣称吞吐已经合格。

主机测试不执行真实ESP-SR DSP，也不模拟ESP32双核调度。通过的结果证明交接顺序、样本和保护语义；实际持续吞吐、声学效果、取消后的恢复及资源仍待设备复验。短项通过后再继续C远端／近端／双讲声学验收，D插话保持关闭。
