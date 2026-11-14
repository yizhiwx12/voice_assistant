# P4.1 内部 RAM 不足：修复与验收

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

更新：2026-11-10。适用硬件 ESP32-S3 N16R8，软件 ESP-IDF 6.1、ESP-SR 2.6.1。后续真机已确认 LVGL 迁移生效，连接后 internal_free=80291、internal_largest=31744、alloc_failures=0；新发现的 AFE 内部容量配置错误和服务端全局停顿另见[本轮修复记录](p4_frontend_ringbuffer_and_server_stall.md)。下文保留首次内存修复实现及历史产物，新固件以新记录为准，长时共存验收仍待通过。

## 问题与证据

本地 AFE/VAD 接入后，用户发现开始录音即断线，后续连 WebSocket 任务也无法创建：

| 现场 | 已取得证据 | 判断边界 |
| --- | --- | --- |
| 录音约 34 帧后失败 | 设备 `internal=1279`、PSRAM 约 7.7 MB；发送最长约 223 ms，随后 `transport_poll_write(0)` | 内部 RAM 极低，设备发送先失败；不能仅凭这一行断言所有历史网络问题都来自内存 |
| 对应服务端 | epoch `89010248`，`category=device_transport_closed`，queue=19，最老音频约 110 ms，无 ASR 错误 | 服务端观察到设备断开，未主动以 ASR 超时或上行媒体落后终止此连接 |
| 后续连接失败 | `Error create websocket task`；清理后 internal=18195、PSRAM 约 7.9 MB | SDK 6144 字节任务栈/任务控制块创建失败；清理后的总量不能代表失败时的连续可用块 |

N16R8 已启用 16 MiB Flash 与 8 MiB PSRAM。Flash 用于程序、模型和数据存储；此次利用已有 PSRAM，释放内部 RAM 给任务、网络和 DMA。总 heap 很大也可能因内部 RAM 或最大连续块不足而失败。目前没有足够证据确认存在内存泄漏。

## 已实施的分配调整

| 内容 | 实现与目的 |
| --- | --- |
| LVGL 固定池 | 通过上游 `LV_MEM_POOL_ALLOC` / `LV_MEM_POOL_INCLUDE` 接口，将 65536 字节 TLSF 池放入 `SPIRAM\|8BIT`，不修改 managed component；保持原池容量，跨重新初始化复用 |
| 池分配失败 | 明确记录并停止初始化，不回退抢占内部 RAM |
| 普通 malloc 偏好 | `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 从 16384 调为 1024；较大的普通分配优先外部 RAM，失败时仍遵循 IDF 分配器的回退规则 |
| 内部保留区 | 保持 `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`，给显式内部/DMA 分配和任务栈等使用；该值不是某个业务的专属额度 |
| 必需内部内存 | LCD/I2S DMA 缓冲、SDK WebSocket 任务及其他明确需要内部 RAM 的分配保持其要求；本轮不改 Wi-Fi/LwIP 专用分配策略 |
| 已有 AFE 外部分配 | 保留 `AFE_MEMORY_ALLOC_MORE_PSRAM`、PSRAM feed adapter、输入队列及两条 8192 字节工作栈 |
| 前端启动预算 | AFE/模型/队列建立后、工作任务创建前，检查内部可用量至少 48 KiB 且最大连续块至少 12 KiB；不满足时记录 `internal_budget`，销毁 AFE、卸载模型并释放缓冲，手动 raw 录音仍可工作，界面显示本地检测异常 |

48/12 KiB 是本阶段启动预检门槛，后续任务创建和 Wi-Fi 动态分配仍会消耗内存，且快照后其他任务可继续分配，不能将通过预检视为稳定性保证。未对访问要求不明的 NSNET3/DL 预编译工作区强制搬迁；应用和模型分区没有因此扩大。

## 新诊断的读取方式

`main/app/memory_diagnostics.*` 在显示、Wi-Fi、模型、AFE、音频任务、WebSocket 初始化和清理等阶段输出：

```text
memory: stage=... internal_free=... internal_min=... internal_largest=... dma_free=... dma_min=... dma_largest=... psram_free=... psram_min=... psram_largest=... alloc_failures=... hook=1
```

- `free` 是当前可用量，`largest` 是可供单次分配的最大块；先同时查看二者。
- `min` 为 IDF 按区域累计的最低值，可能并非同一瞬间的总量。
- DMA 是内部内存能力的一个重叠视图，不能把 DMA 数字与 internal 数字相加。
- `alloc_failures` 是分配请求失败次数，可能包含后续已成功回退的请求，不等于业务失败次数。
- 分配失败回调使用内部 DRAM 中的固定记录与 ESP-IDF 短临界区，同步计数和第一条请求的字节数、能力掩码、函数名；在普通任务的下一次阶段日志中输出 `allocation_failure`，不在回调或临界区内查询堆、记录日志或分配内存，不使用普通互斥锁。第一条失败记录保持不变。

串口 `?` 会新增 `stage=manual`。首次会话错误记录 `stage=conversation_failure`；WebSocket 启动失败前先输出 `ws_start_failed`，清理后输出 `ws_after_stop`，便于区分失败时与回收后的状态。没有在音频逐帧发送路径增加堆查询或阶段日志。

## 软件验证

以下检查不连接硬件或云端，不改服务端和凭证：

| 检查 | 结果 |
| --- | --- |
| LVGL PSRAM 池 | 4 个分配/错误场景通过；实际 LVGL TLSF 100 次初始化、分配、扩容、耗尽、释放和反初始化复用同一池；主机符号检查验证旧静态池消失 |
| 内存诊断模块 | 8 场景通过，包含能力区域、连续块、注册重试、名称边界、并发回调及延迟日志；回调期间禁止堆查询、日志和 C++ new |
| 本地前端生命周期 | 14 场景通过，新增总量不足/连续块不足时不创建任务、释放模型/AFE/队列/适配器，重复清理安全 |
| 既有固件行为 | 会话 8 场景、传输 7 场景、UI 71 项、PCM 适配 5 场景/6 块长、MAX98357A 生命周期检查通过 |

检查日志保存于 `build/p4_memory_check_*.log`。ESP32-S3 / IDF 6.1 构建通过，最终日志为 `build/p4_memory_build_verified.log`。前两次构建暴露的 Xtensa 原子兼容与静态库 hook 抽取顺序问题均已修正；分别使用内部 DRAM 的专用 ESP-IDF 短临界区，以及显式链接 `voice_lvgl_pool_alloc` 符号，未删除安全检查以绕过编译。

实际链接核对已通过，结果保存为 `build/p4_memory_verification.json`，前后归档统计为 `build/p4_memory_before_archives.json` / `build/p4_memory_after_archives.json`：

| 实际构建证据 | 结果 |
| --- | --- |
| LVGL 内部 DIRAM 占用 | 66128 → 592 字节，减少 **65536 字节**；最终 map 中旧 `work_mem_int` 符号消失 |
| main 内部 DIRAM 占用 | 19224 → 19308 字节，诊断记录与池指针等增加 84 字节；不能将单模块静态变化等同于设备运行时 free 增量 |
| 诊断固定记录/同步锁 | 分别位于内部 DRAM `0x3fca4618`（68 字节）、`0x3fca465c`（8 字节） |
| 编译数据库 | `display/lvgl_memory.cpp`、`app/memory_diagnostics.cpp`、`audio/local_audio_frontend.cpp` 均进入实际编译；真实 LVGL 编译命令包含池 hook |
| 实际 sdkconfig | 普通分配门槛 1024，内部保留 32768，与 defaults 一致 |

首次内存修复产物（历史记录，当前固件见后续修复记录）：

| 文件 | 长度与 SHA256 |
| --- | --- |
| `build/voice_assistant.bin` | 2703360 字节，3 MiB 应用分区剩余 442368 字节（约 14%）；`cc6cdbdd1d5c50192b6d5f3c544f28f297ec0fcd73cf9cd68b0ceee33710ecda` |
| `build/partition_table/partition-table.bin` | 3072 字节；`050842b5a60cd54e31aebda03becefa83d17061068418dec5f8993513d1447f6` |
| `build/srmodels/srmodels.bin` | 288069 字节；`2485a4caf5754547459622a0ba126428c3dab1cef8bf57ba24e463586d2232ee`；完整烧录表仍含 `0x310000` 模型条目 |

以上为此次构建记录。重新构建后以实际产物为准，真机资源余量尚待测试。

## 需要用户完成的真机验收

在 ESP-IDF 终端进入 `C:\esp-project\voice_assistant`，保持使用当前新版实时服务端，完整烧录当前构建：

```powershell
idf.py -p COM7 -b 115200 flash monitor
```

串口不同则替换 COM7；模型与分区仍须和应用匹配。无需擦除 Flash，也不需要更改云模型/密钥。

1. 从启动保留串口日志，确认 `LVGL fixed TLSF pool: 65536 bytes in PSRAM`；检查模型校验和 `VAD observation ready`。若出现 `internal_budget`，这代表前端保护触发，不能标记本地 VAD 验收通过。
2. 连接就绪、尚未录音时输入 `?`，保留 `heap=...`、`local_frontend ...` 与 `memory: stage=manual ...`。重点记录 internal 的当前值、最低值、最大块和失败请求。
3. 连续完成至少 10 轮 `r → 讲话 → s → 播放`，再按 `?`；穿插播放中 `c` 与空闲 `n` 重连至少 5 次，比较相同就绪状态下的内存，观察是否持续下降。单次下降不能直接判定泄漏。
4. 待机至少 5 分钟，期间穿插对话；测试约 20–25 秒持续讲话后手动 `s`，另做风扇噪声/静音对照并记录自动 30 秒上限行为。P4.1 仍是手动 raw 上行，不应期待本地 VAD 自动判停。
5. 如再失败，保留首次失败前后的阶段内存日志、分配失败详情和相同 epoch/turn 的服务端日志；分别判断本地分配、网络停顿、Pong 或云端错误。

当前完成的是软件内存布局修复与保护。设备上的实际内部 RAM 增量、任务创建、VAD 共存和长时稳定仍需上述日志验收；P3 上行到达停顿与 Pong 超时继续独立跟踪。
