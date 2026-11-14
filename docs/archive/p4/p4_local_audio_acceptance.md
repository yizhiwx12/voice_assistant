# P4.1 本地语音前端部署与真机验收

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

更新：2026-11-10。**A及上一版B852.625秒数字／资源实机通过保留；新B收尾声学聚合统计、任务分项及栈优化软件／构建通过，尚未烧录或验收，当前操作见[新批验收](p43b_acoustic_statistics_acceptance.md)。** 主观取消／声学范围仍待补，calibrated=0；AEC／NS／AGC和插话关闭。本文保留P4.1／P4.2历史方法及实机记录，P4.2专项、英文／空识别／断连暂缓事项继续保留；总体见[P4总结](p4_issue_summary.md)和[P4清单](p4_local_audio_checklist.md)。

用户后续验收确认：安静环境各项功能正常，30秒无人讲话返回空识别，有讲话则到期自动提交并返回语音，长时待机正常。以上手动功能基线记为用户实测通过；本次未给出待机分钟数、轮次或资源趋势，不替代风扇／弱音声学、专项取消重连和量化资源记录。30秒自动提交不是VAD判停。

此前空识别提示被Ready页覆盖的问题已修复，并增加云端结果／解码PCM诊断；固件构建及203项服务端离线测试通过。epoch2209324224曾确认真正空最终及低PCM电平，随后关风扇恢复识别；提示实际可见性和风扇干扰下的改善效果仍待验证。证据见[空识别与显示排查](p4_empty_asr_result.md)。

## 本次接入的范围

固定 ESP-SR `2.6.1`，选择 `vadnet1_medium`，单麦克风输入为 PCM16、16 kHz。AFE 使用动态查询的 feed/fetch 块长，固定容量适配器将连续输出重组为 320 样本（20 ms）；观测路径不插入 `vad_cache`，避免重复音频。NS、AGC、AEC、WakeNet 均关闭。当前 VAD speech/noise/delay 参数均为 128 ms，只用于状态观测，与 P4.2 待调试的 600–800 ms 发言结束判定不同。

P4.1历史阶段选择“原始PCM手动录音＋本地VAD观测”：原始采集先交给 `r/s`，再非阻塞投递AFE队列，当时未将AFE输出接入云ASR。后续P4.2已接入AFE输出、尾部排空、预录和握手缓存，并实现半双工自动起止；当前模式见下方P4.2记录。

本地观测入口队列为 8 个 20 ms 帧；ESP-SR 内部环形缓冲保持组件默认 50 个 AFE 帧，以容纳 VAD 历史和待处理数据，两者不可混淆。队列满或模型/AFE 出错会锁存明确错误，屏幕显示“本地检测异常”，不会静默丢帧后继续声称 VAD 正常。当前显式手动输入模式仍可工作。关闭 `menuconfig → Voice Assistant → Manual raw PCM input with local AFE/VAD observation (P4.1)` 可做原始 P3 对照。

## 模型与部署

来源：[ESP-SR 2.6.1 官方组件](https://components.espressif.com/components/espressif/esp-sr/versions/2.6.1)。组件声明支持 IDF ≥ 5.0；本项目具体使用 IDF 6.1 / ESP32-S3，构建结果单独记录。依赖精确解析版本保存在 `dependencies.lock`。

| 项目 | 内容 |
| --- | --- |
| 模型 | `vadnet1_medium`，仅此模型 |
| 分区 | `model`，data/spiffs，起始 `0x310000`，容量 1 MiB |
| 应用分区 | 保留 factory 起始 `0x10000`、容量 3 MiB |
| 模型镜像 | `build/srmodels/srmodels.bin` |
| 部署记录 | `build/p4_model_manifest.json`，含版本、模型文件范围、镜像长度、SHA256 和完整烧录文件表 |
| 固件内校验 | 启动时读取实际 model 分区，对精确镜像长度做 SHA256，再交由 ESP-SR 加载并核对名称 |
| 主任务栈 | 8192 字节；启动日志记录剩余水位，不将增大栈空间视为实机已通过 |

构建会检查模型包结构、分区容量和重叠、完整烧录表中的模型地址。产物变化会重新生成固件使用的校验信息。

## 需要你完成的操作

在 **ESP-IDF 终端**进入项目。服务端继续使用现有配置，不需要申请新云密钥，本地 VAD 不调用云：

```powershell
cd C:\esp-project\voice_assistant
.\.venv-voice\Scripts\python.exe server/voice_server.py --mode realtime --profile realtime_default
```

在另一个 ESP-IDF 终端完整烧录：

```powershell
cd C:\esp-project\voice_assistant
idf.py -p COM7 -b 115200 flash monitor
```

COM7 为当前项目设置，实际串口不同时替换。第一次必须使用完整 `flash`，使 bootloader、分区表、应用和模型成套更新。只用 `app-flash` 不会部署新增模型。无需执行 `erase-flash`；保持现有接线、增益和音量，先便于与 P3 对照。

启动日志应依次看到以下信息；以本次 `build/p4_model_manifest.json` 为实际校验依据：

```text
sr_model: Verified ESP-SR=2.6.1 model=vadnet1_medium bytes=... offset=0x310000 sha256=...
sr_model: Loaded model=vadnet1_medium count=1
local_frontend: VAD observation ready: model=vadnet1_medium mono16k feed=... fetch=...; manual_uplink=raw_pcm; NS=off AGC=off AEC=off
local_frontend: vad=silence ... (observation only)
assistant_app: Startup main stack_free=... internal=... psram=...
```

若出现 `Missing model partition` 或 `Model checksum mismatch`，先核对是否烧录了同一轮构建的完整文件；不要将其当作风扇导致的误判。若加载失败，保留完整错误段和 `?` 输出。

## 按顺序测试并记录

先不按 `r`。本地 VAD 会持续运行，但不会上传空闲时的音频；屏幕在就绪或回答完成时显示“VAD 观测：静音/人声”。按 `?` 输出原有上行统计及 `local_frontend` 指标。

| 测试 | 操作 | 需要记录 | 状态 |
| --- | --- | --- | --- |
| 模型部署 | 完整烧录后启动，按 `?` | 版本、名称、SHA256、ready=1/error=0、实际 feed/fetch 长度 | 最新真机日志已取得，模型加载与50帧AFE持续运行确认 |
| 安静环境 | 不按 `r`，安静 60 秒 | 静音状态、无故切到人声的次数；不应新增云回合 | 待测 |
| 正常讲话 | 距麦克风约 30–50 cm，说 5 句，句间停 2 秒 | 每句是否切到人声及返回静音；无录音自动开启 | 待测 |
| 弱音与短句 | 固定距离，说短词/轻声各 5 次 | 漏检次数、距离、音量和环境 | 待测 |
| 风扇背景声 | 先仅开风扇 60 秒，再说 5 句 | 单独噪声误判、噪声中讲话漏检、风扇距离/档位 | 待测 |
| 手动对话 | `r`，等显示录音再说话，`s`；至少 5 轮 | 首尾音、识别文字、播放、VAD 及队列；仍按原操作提交 | 待测 |
| 取消和重连 | 回复中按 `c`，再 `r/s`；空闲按 `n` 后再录 | 旧声音不恢复、回合不串、VAD 仍运行 | 待测 |
| 长时共存 | 待机 ≥5 分钟，期间数次对话，前后按 `?` | 内部 RAM/PSRAM、栈、AFE 队列、处理耗时、重启或错误 | 待测 |
| 播放期间观测 | 无人说话时播放回答 | 记录回声是否被 VAD 判成人声；当前 AEC 关闭，此项不触发新录音 | 待测 |

`feed_max_us` 是 feed 调用的墙钟耗时，`fetch_max_us` 包含等待输出的时间，不能直接称为 CPU 利用率。队列、调用耗时与栈水位需结合真实双向 Opus、Wi-Fi、显示负载评估。实际 CPU 占用和声学延迟仍需实机测量。

建议提供一份从启动到测试结束的串口日志，以及每项“正常/异常”和误判、漏检次数。异常时同时保留 `?` 与同一 epoch/turn 的服务端日志；日志可放 `build/` 或 `server/data/`，隐去 Wi-Fi、个人对话和其他敏感信息。此前 P3 网络空档/Pong 问题继续独立跟踪。

## 软件验证记录

2026-11-10，以下为最初 P4.1 观测基础的软件记录；本次内存修复另见 [内存修复与验收](p4_memory_budget.md)。构建过程中未烧录、未调用云服务，服务端和凭证文件未修改：

| 检查 | 结果与证据 |
| --- | --- |
| ESP32-S3 / IDF 6.1 | 完整构建与最终增量构建通过，日志 `build/p4_build.log`、`build/p4_build_final.log` |
| 编译数据库 | `local_audio_frontend.cpp`、`sr_model_store.cpp` 已进入 `build/compile_commands.json` |
| 模型部署工具 | 12 项通过，涵盖截断、错误偏移、分区重叠/容量、漏烧模型和内容变化；`build/p4_model_manifest_tests.log` |
| 实际模型镜像 | 288069 字节；完整烧录清单含 `0x310000 → srmodels/srmodels.bin` |
| 生产前端生命周期模拟 | 12 场景通过，涵盖初始化/任务创建失败、普通等待、持续无输出、队列满、超时保留资源与重复启停；`build/p4_local_frontend_tests.log` |
| PCM 块长适配 | 5 场景、6 种块长通过，检查样本顺序、无重复、无补零；`build/p4_pcm_adapter_tests.log` |
| 控制器生命周期 | 8 场景通过，新增 VAD 观测不创建云回合、不影响手动会话；`build/p4_conversation_tests.log` |
| 状态与 UTF-8 文本存储 | 71 项通过，包含 VAD 变化刷新及错误隔离；`build/p4_ui_text_tests.log` |

最初 P4.1 产物（历史记录，不是本次内存修复构建）：

| 文件 | 字节数 / SHA256 |
| --- | --- |
| `build/voice_assistant.bin` | 2700976 字节，3 MiB 应用分区剩余约 14%；`790485ab0848c1d5b62ea3c970929ea95e1bce2d33a532a31a1cb7153231bfc5` |
| `build/partition_table/partition-table.bin` | `050842b5a60cd54e31aebda03becefa83d17061068418dec5f8993513d1447f6` |
| `build/srmodels/srmodels.bin` | 288069 字节；`2485a4caf5754547459622a0ba126428c3dab1cef8bf57ba24e463586d2232ee` |

重建后应用哈希可能变化，以实际产物为准。上表说明软件实现和打包可核查，无法代替 VAD 声学效果、真实时序、CPU/内存和长时稳定验收；真机表尚未勾选。

## 进入下一步的条件

历史顺序为P4.1→P4.2→P4.3参考／对齐／AEC／插话，既有基线和专项保留。上一版B数字／资源已通过；新B收尾保存已通过候选的分布，软件／构建通过，按[新批验收](p43b_acoustic_statistics_acceptance.md)烧录后做固定摆位重复远端单讲，补主观取消及任务资源，不重做14分钟数字测试。旧26次通过无分布、被拒45ms不能作校准；取得可解释候选范围后C AEC验回声降低／近端／双讲，再D插话。

## P4.2 软件实现及实机待办

新固件串口命令：`a` 显式开启半双工自动监听，`m` 退出自动监听回手动；手动 `r/s/c/n/?` 保留。自动模式在至少300 ms VAD静音门后才接受讲话候选，保留约200 ms AFE PCM预录；服务端ASR准备期间缓存最多约2秒，准备完成后按20 ms节拍上行。默认静音700 ms结束发言，30秒是已讲话后的硬上限；无人讲话不会创建云端轮次。助手识别、生成和实际播报期间暂停监听。未接入AEC，不能边播边听或自动插话。

自动路径使用ESP-SR连续AFE输出而不是原始麦克风PCM；服务端只在音频流结束和 `speech.end` 后提交整轮识别。`s` 可请求对齐当前采集位置并排空尾音；录音中`c`取消当前回合、不提交LLM，并等待 `speech.cancelled` 后才可新建回合，且退出自动监听；回复中`c`手动打断。若AFE输出失效、缓存溢出或ASR启动超时，自动模式取消该轮并显示错误，不回退或重放原始PCM。P4.1手动路径仍是原始PCM，便于对照。

此前软件检查已通过：AFE输出值及512→320样本时间线、预录连续性/环绕/缺帧检测、自动端点13场景、生产控制器30场景、UI 89项、服务端209项，ESP-IDF 6.1固件完整构建通过。固件 `build/voice_assistant.bin` 为2,709,536字节，SHA256 `00b3290def4d30d8d3d394a4fa1d8b53ea9c7814ee8c781fda5443f446e7d412`；3 MiB应用分区余14%。该轮离线测试未调用真实云服务，助手未操作烧录；之后用户提供了下方实机证据。

实机使用时须将本工作区内更新后的实时语音服务与该固件配套运行，以支持 `speech.cancel`；旧版服务不支持该取消消息。先从项目根目录启动服务：

```powershell
.\.venv-voice\Scripts\python.exe server/voice_server.py --mode realtime --profile realtime_default
```

另开 ESP-IDF 终端执行完整烧录（把 COM7 换成实际端口）：

```powershell
idf.py -p COM7 -b 115200 flash monitor
```

完整烧录清单包含 bootloader、分区表、应用和 model 分区。启动默认是手动模式，先按`a`启用自动监听。先测安静至少30秒不建立云端回合，再测短句、句中停顿、700ms静音提交、`s`尾音、`c`握手取消及`m`回手动；重新做P3原始手动`r/s`对照。用风扇关闭的安静场景建立基线，记录首字/尾音、误触发、漏检、服务端final/LLM调用数及 `?` 前后 RAM/AFE 队列；硬件接受前，不将P4.2标为完成。

## P4.2 本轮实机记录与后续安排

2026-11-10用户提供两端日志，并反馈中文识别较好、英文较弱；认可当前基本交互，要求英文效果和偶发断连先保留记录，不立即处理。P4.2状态由“待烧录”更新为“实机基本交互已验证，专项验收未完”，不直接勾选全部验收项。本次整理没有修改固件、服务端实现或本地配置。

证据来源为本次对话附件：

- 服务端：`03210b52-9a80-4e55-863b-630e4f3766f3/已粘贴的文本.txt`。
- 设备：`2b526b34-fa26-4fc7-8b61-8c60dc0366ba/已粘贴的文本.txt`。
- 后续用户粘贴的turn 10／11两端日志，服务端时间16:42:16与16:43:07，同一epoch `953293006`。

启动显示应用编译时间15:56:15、ESP-IDF6.1、16MiB Flash／8MiB PSRAM，ESP-SR2.6.1模型校验加载成功。AFE为单麦VAD-only，NS／AGC／AEC关闭，自动路径用AFE PCM。日志没有完整复述UART操作，不据此推断取消、切换或重连已验收。

| 轮次 | 云端结果 | ASR实际PCM时长 | PCM RMS | 本轮最大到达间隔 | 本轮最大相对媒体落后 | 说明 |
| --- | --- | --- | --- | --- | --- | --- |
| 1–4 | 有文字，分别15／11／8／12字 | 4.26／3.50／2.84／3.40秒 | 151／133／68／62 | 156／109／188／110ms | 105／55／145／63ms | 自动起止及中文识别取得证据 |
| 5 | 空final | 1.20秒 | 45 | 110ms | 70ms | VAD人声约288ms |
| 6 | 空final | 1.30秒 | 35 | 141ms | 103ms | VAD人声约384ms |
| 7 | 空final | 1.40秒 | 36 | 141ms | 87ms | VAD人声约480ms |
| 8 | 空final | 1.12秒 | 32 | 94ms | 54ms | VAD人声约192ms |
| 9 | 有文字，7字 | 2.08秒 | 148 | 141ms | 95ms | 空结果后同连接恢复 |
| 10 | 有文字，14字 | 4.08秒 | 118 | 218ms | 160ms | 后续轮次仍正常 |
| 11 | 传输保护中止 | 未取得最终识别，不换算完整PCM时长 | 无最终统计 | 1391ms | 1273ms | 超过750ms媒体预算，uplink_backlog |

turn 1–10正常结束：6轮非空、4轮空；这不是固定语料识别准确率，不能写成60%准确率。turn 5–8的设备处理帧×320与服务端PCM样本数一致，发送成功；云端正常空最终而非网络报错。短暂VAD人声加200ms预录、700ms静音形成1.1–1.4秒短轮，指向短候选／弱音／判停或误触发方向，未回听音频，不能定因。成功与失败轮的音量范围有交叠，不按单一RMS门限判断识别成败。

turn 11是另一类上行停顿：服务端queue=0、worker=waiting、loop_lag_max=25ms、asr_reason=none；设备send_max_us=223690、队列11、内部空闲约79KB，失败诊断alloc_failures=0，最大内部块31744。媒体滞后检查在入队前执行，所以空队列也可触发。不能从跨机器日志确定设备写失败与服务端关闭的先后，也不能确认具体Wi-Fi／TCP／调度原因。连接级receive_gap_max_ms=350484含跨轮空闲，不作为录音中断网时长。

后续安排：

- 保留中文基本交互、手动原始PCM与自动AFE两条路径；NS／AGC／AEC目前仍关闭。
- 英文识别、偶发短候选空识别、网络停顿暂缓优化，保留配置与实时保护，不标记已修复。风扇／弱音鲁棒性专项另留。
- 首尾音、慢握手、取消／重连、播放抑制与量化资源仍待专项验证，可随P4.3各批次回归。
- P4.3 A参考／资源和上一版B852.625秒数字／资源实机通过保留，报告`build/p43b_hardware_observation.json`；新B收尾统计／任务分项／栈优化软件构建通过，尚未烧录和新批验收。按[新批验收](p43b_acoustic_statistics_acceptance.md)取得重复远端单讲候选分布、主观取消及任务资源，calibrated=0；之后C AEC、D插话。各软件报告的未烧录状态仅表示其交付时点，不覆盖其后独立硬件证据。
