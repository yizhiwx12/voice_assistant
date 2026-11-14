# 2026-11-12：偶发开机 AEC 配对失败与 QIO/DIO

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

## 两次设备启动的证据

附件 `199344ed-dc24-4b73-861a-5d88d64666f8` 和 `01826439-c0a4-416e-9e0d-b9c3d96f187e` 使用同一个 ELF `cb8ebcbd6...`。两次都完成 Flash QIO/80MHz 初始化、应用映像加载、8MB Octal PSRAM/80MHz 自检、VAD 模型校验和 AFE 创建。新的 `VAD actual` 和 `idle_feedback=1` 日志均已出现，说明前一轮包含头文件修正的固件已由用户构建并运行。

第一份启动在约 6.5 秒出现：

```text
AFE/VAD unavailable: reason=microphone_unavailable error=ESP_ERR_TIMEOUT
aec_reference_failed reason=pair_queue_full error=ESP_ERR_TIMEOUT
```

这里的 `microphone_unavailable` 是原来通用 `invalidate()` 的固定字符串，实际触发原因是配对队列满：

- 麦克风从启用至首次读取仅 70us，102 次读取全部完成。
- DMA `overflow=0 invalid=0`，没有捕获到时钟故障。
- 前端已 feed/output 28672 个样本，故障时前端输入队列为 0，`frontend_waits=0`。
- 最近参考服务快照里配对队列为 7 帧，故障时已达容量 10 帧；参考转换仍积压 7 个块，时间轴健康。
- 最近服务快照比故障时刻早约 69ms。该快照不等于整段期间的任务运行追踪，不能据此推算精确 CPU 占用或最长抢占时间。
- 故障发生于首次 Wi-Fi 连接重试/认证期间，网络约 11.7 秒才获得 IP。

第二份约 8.8 秒获得 IP，所给启动片段没有配对失败。两份片段都没有覆盖后续播报，不能据此判断先前的播报期 WDT 或插话灵敏度是否已解决。

## 判断和修改

确定的问题是：连续音频启动后，播放/参考配对服务未能及时消化队列，AEC 因安全保护而停用。并非模型加载或麦克风硬件初始化失败。

目前参考 owner 在 CPU0/优先级 6，Wi-Fi 驱动在 CPU0/优先级 23；初次连接的竞争与故障时段一致，是较强的原因假设。现有日志不能证明具体哪项 SDK 工作导致延迟，也不能单独归因于最多 8ms 的 idle 主动等待。

本轮在 `main/app/voice_assistant_app.cpp/.hpp` 调整首次启动顺序：

1. AEC 配置在 Wi-Fi 处于 `Connecting` 时，暂不安装 UART 命令输入、不启动控制器和连续音频。
2. 应用现有 100ms 循环继续刷新 UI；屏幕显示麦克风准备中、正在连接 Wi-Fi。
3. Wi-Fi 成功取得 IP，或首次连接已失败/未配置时，执行一次原有音频初始化。后两种状态保留离线原始录音初始化路径。
4. 启动后断网由原有逻辑处理，不再次初始化音频。延迟启动失败仍执行原先的致命启动错误策略，不每 100ms 重试部分分配的资源。
5. 非 AEC 配置保留即时初始化方式。

这是对初次联网竞争的针对性缓解，不保证消除运行期间的重连竞争。队列容量、配对年龄、核心、优先级、idle 等待、声学参数和 D1/D2 配置均未改动。

另让 `LocalAudioFrontend::invalidate()` 接收真实原因，`failAecReference()` 传递 `pair_queue_full` 等原因，避免再把参考配对失败显示为麦克风不可用；原来的麦克风错误调用仍使用默认原因。首个错误锁存规则不变。

## QIO 和 DIO

| 模式 | 地址/数据阶段并行位数 | 同为 80MHz、STR 时的数据阶段理论带宽 |
| --- | --- | --- |
| DIO | 2 | 20MB/s |
| QIO | 4 | 40MB/s |

这是 Flash 访问总线宽度，实际程序性能还受缓存、命令/地址开销等影响，不等于整个应用快两倍，也不决定串口烧录波特率。QIO 需要 Flash、接线和 Quad Enable 支持；DIO 兼容性通常更宽。两者都不改变 Flash 容量。

当前实际运行 QIO/80MHz。ROM 先打印 `mode:DIO`，随后二级 bootloader 打印 `Enabling default flash chip QIO`、`SPI Mode : QIO`，应用打印 `flash io: qio`，是正常的切换顺序。ESP-IDF 6.1 的 `components/esptool_py/Kconfig.projbuild` 明确说明 QIO/QOUT 镜像先按 DIO 加载，再由 bootloader 启用 Quad；因此 `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` 和隐藏字符串 `CONFIG_ESPTOOLPY_FLASHMODE="dio"` 同时出现也正常。

Flash QIO/DIO 与这里的 Octal PSRAM 是两个独立配置。当前日志没有 Flash 读错误、映像校验失败或 PSRAM 自检错误，暂时保留 QIO/80MHz，不把切回 DIO 当成配对队列故障的修复。

用户随后要求统一使用 QIO。根 `sdkconfig` 已选择 `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y` 和 80MHz；已在 `sdkconfig.defaults` 同步显式选择 QIO/80MHz，使今后新建配置也采用该模式。`build`、`build/aec-test` 和 `build/barge-in-test` 的配置来源均为同一个根 `sdkconfig`；旧构建的 DIO 头文件/二进制须由重新配置和构建更新，不手动修改生成产物或 `sdkconfig.old` 历史备份。用户仍只使用 `barge-in-test`，执行下方构建前先执行 `idf.py -B build\barge-in-test reconfigure`。该配置变更只做静态核对，没有执行固件构建、烧录或串口操作。

## 验证与用户命令

已通过主机 `check_local_frontend.py`（mono/SR/FD）及 `check_audio_service_reference.py`（三个变体），包含原因传递与首错保留检查。应用启动门控另用完整生产应用源码和头文件的临时主机检查验证：替换外设/RTOS，覆盖 AEC、非 AEC、关闭实时语音三种配置，检查连接等待期间持续刷新、成功/失败/未配置时启动一次、后续重连不重建、UART/控制器启动失败不重复分配。主机编译启用 `-Wall -Wextra -Werror`，临时验证文件已清理。这些检查不证明目标平台编译、实际调度或声学效果。

用户执行构建和烧录；仍使用根 `sdkconfig` 与 `build\barge-in-test`：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build\barge-in-test build
idf.py -B build\barge-in-test -p COM7 -b 115200 flash monitor
```

如需语音服务，在另一终端执行：

```powershell
cd C:\esp-project\voice_assistant
.\.venv-voice\Scripts\python.exe server\voice_server.py --mode realtime --profile realtime_default
```

先检查启动顺序：`Audio startup deferred` → Wi-Fi 得到 IP/连接重试结束 → `Audio startup resumed` → `AFE/VAD ready`。等 `Voice:` 命令提示出现后才输入串口命令。无需先测插话：开机静置约 15 秒，再按一次 `?`，确认 AEC 仍 active，且没有 `pair_queue_full`。设备结果待用户提供。
