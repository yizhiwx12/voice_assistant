# P4.1 AFE 缓冲耗尽与服务端事件循环停顿

> 历史归档（2026-11-12 整理）：保留当时的实现、证据与待验状态。正文中的“当前”“下一步”、旧构建目录和命令均属于该批记录；日常操作请查[操作指南](../../operations.md)，最新状态请查[当前进度](../../realtime_voice_progress.md)，返回[文档索引](../../README.md)。归档不改变未验收事项。

更新：2026-11-10。对应用户附件 `8919e3f5-7579-467f-bb5f-9b4e34bcbdb9/已粘贴的文本.txt` 与同轮服务端日志，epoch `108072033`。

## 本轮证据和结论

此次有两个独立故障，原来的内部 RAM 极低问题已明显改善：

| 观察 | 证据 | 结论 |
| --- | --- | --- |
| 内存修复已部署 | 启动日志确认 LVGL 65536 字节池在 PSRAM；连接后 internal_free=80291、internal_largest=31744、alloc_failures=0；后续失败时 internal_free=86579 | 当前故障没有此前片内 RAM 仅约 1 KiB、WebSocket 任务创建失败的证据 |
| 本地 VAD 很快失效 | 启动约 4.69 秒出现 `Ringbuffer of AFE(FEED) is full`，随后 `reason=afe_feed`；feed_samples/output_samples 都是 4096 | AFE 内部环形缓冲配置与 VAD 历史保留量冲突，可从实际库验证 |
| 手动上行曾正常 | 第 1 轮 499 个有效音频帧、500 个包；设备队列峰值 1，send_max_us=5477；服务端样本数 159680、无媒体落后 | 本地观测失败后，显式 raw 手动录音路径仍能上传；这不代表完整对话和长时稳定已通过 |
| 服务端整体停顿 | `loop_lag_ms=273588`，之后同时出现多个握手错误；第 2 轮尚无音频到达 | Python 事件循环约 4 分 33 秒未运行，能解释设备 PONG 超时及新连接握手无响应 |
| 本机检查 | 运行中服务端的 `127.0.0.1:8000/ping` 一次 3 秒超时，稍后返回 200/OK、约 47 ms | 故障范围包含电脑本机服务端，无需只依赖热点或设备 Wi-Fi 假设；检查未调用任何云 API |

## AFE 的确定性配置错误

`main/audio/local_audio_frontend.cpp` 曾覆盖 `afe_ringbuf_size=8`。实际 ESP-SR 2.6.1 单麦库的 feed/fetch 块长都是 512 样本，内部容量因此为：

```text
8 × 512 samples × 2 bytes = 8192 bytes
```

当前 `vad_min_speech_ms=128`、`vad_delay_ms=128` 所需历史保留上限同样为：

```text
(128 + 128) ms × 16000 samples/s × 2 bytes = 8192 bytes
```

实际库 `afe_fetch_out` 逐步累计这项保留量；`afe_feed` 需要“历史保留量 + 下一帧”的空间。8 块后历史占满容量，第 9 块需要 9216 字节，即使消费者已读走输出也无法满足。日志恰好在 8×512=4096 样本后失败，并非简单的工作任务优先级或空闲 RAM 不足。

修复恢复 `afe_config_init` 的官方默认 **50 帧内部缓冲**，不再将其压缩为 8。相较之前增加约 42 KiB 缓冲容量，由既有 MORE_PSRAM 分配策略管理；这不是设置等待 50 帧才输出。项目自己的非阻塞入口队列仍为 **8×20 ms=160 ms**，它和 ESP-SR 内部环形缓冲职责不同。

保留 `feed <= 0` 的真实错误保护，不通过忽略失败、无限重试或宣称 VAD 正常掩盖音频丢失。原测试使用无界输出队列，未覆盖 VAD 历史保留空间，是此次漏检原因；回归需模拟有界容量及历史保留，旧 8 帧配置应复现失败，默认配置在持续输入后保持 ready。

## 服务端全局停顿的处理

已确认的是事件循环长时间停顿，尚无完整线程栈证明历史停顿的唯一原因。此前 `server/voice_server.py` 使用同步 `logging.StreamHandler`，`voice_asr_summary` 等日志直接在事件循环中写终端。终端选择/快速编辑、终端管道背压或慢输出均可能让写入阻塞，从而同时拖住所有连接、`/ping` 和 WebSocket 心跳。云 ASR 的正常异步等待不会单独造成这一现象。

针对这条已确认存在的阻塞路径，将实时诊断日志改为：

- 固定容量队列、非阻塞入队；后台专用线程写入原日志输出，保持已有格式和安全字段。
- 队列满仅累计丢弃数量，恢复后由后台报告；不在事件循环中调用默认 `QueueHandler.handleError` 写 stderr。
- 日志线程作为 daemon，停止等待有时限；日志组件不无限等待输出线程。Python 标准流最终刷新和操作系统强制暂停等行为仍不在该组件的保证范围内。
- 不延长或关闭 PONG 超时，不将全局停顿伪装为空识别，也不改模型和密钥配置。

回归需要阻塞日志输出时仍能完成本机 `/ping`、WebSocket PING/PONG 和新连接升级，并验证队列上限、恢复和有界停止。这证明日志输出与事件循环隔离，仍不能代替用户现场复验或排除其他同步阻塞来源。

## 验证与后续操作

固件构建已通过，日志 `build/p4_ringbuffer_build.log`；生产前端生命周期 **16 场景通过**，包括 400 个 AFE 帧的连续静音与旧 8 帧配置的失败负对照，日志 `build/p4_ringbuffer_frontend_tests.log`。实际库推导与反汇编保存在 `build/afe_ringbuffer_evidence.txt`。主机模型用于验证容量/历史约束，不能代替运行真实 VADNet 的声学和调度测试。

产物与内存布局核对见 `build/p4_ringbuffer_verification.json`：

| 文件/检查 | 结果 |
| --- | --- |
| 应用 | `build/voice_assistant.bin`，2703392 字节；SHA256 `838bec02ea005bcb0206081872ce4618303ca77b42600dc334c0a0829ce32124` |
| 模型 | 288069 字节；SHA256 `2485a4caf5754547459622a0ba126428c3dab1cef8bf57ba24e463586d2232ee`，完整烧录表仍含 0x310000 |
| 内存布局 | LVGL 原内部池仍已移除，DIRAM 为 592 字节；配置门槛1024/内部保留32768一致，新增前端源在真实编译数据库内 |

服务端完整离线回归 **192 项全部通过，无跳过**，日志 `build/p4_ringbuffer_server_tests.log`。其中新增诊断日志 9 项覆盖阻塞输出期间 `/ping`、WebSocket PONG、新连接升级、有界队列与丢弃汇总、失败输出不回退 stderr、日志格式/长度和正常/异常退出清理。已有会话控制器 8 场景、PCM 适配 5 场景/6 块长亦通过，日志 `build/p4_ringbuffer_check_conversation_lifecycle.log` / `build/p4_ringbuffer_check_pcm_block_adapter.log`。

助手未烧录设备、未重启用户当前服务，也未调用云服务。服务端的业务 profile、凭证、模型和超时配置未修改。

用户需要更新两端：

1. 在服务端终端按 `Ctrl+C` 结束旧进程，再用下面命令启动新版服务。若终端选中了文本，先按 `Esc` 退出选择；修改 Python 文件不会让旧进程自动加载新实现。
2. 在 ESP-IDF 终端进入 `C:\esp-project\voice_assistant`，执行 `idf.py -p COM7 -b 115200 flash monitor`，串口不同则替换。保留应用、分区和模型匹配，不需擦除或填写新密钥。
3. 连接就绪后不按 `r`，等待至少 60 秒并输入 `?`：应看到 local_frontend ready=1/error=0，feed/output 持续增长并明显超过 4096，队列不过载；记录内存和模型校验。
4. 完成多轮手动对话、取消和重连，待机至少 5 分钟。P4.1 仍是 raw 手动上行加本地 VAD 观测，自动端点/AEC 尚未接入。
5. 若再有 Pong 或握手失败，先从电脑访问 `http://127.0.0.1:8000/ping`，同时保留服务端 loop_lag、日志队列丢弃计数和同一 epoch/turn 的配对日志。

服务端重启命令（从项目根目录执行）：

```powershell
cd C:\esp-project\voice_assistant
.\.venv-voice\Scripts\python.exe server/voice_server.py --mode realtime --profile realtime_default
```

启动应看到 `afe_ring_frames=50`；至少等待 60 秒后 `?` 的前端 ready 应仍为1，feed/output 持续增长。本轮主机结果不能替代此项实机验收。

后续仍按 [P4 清单](p4_local_audio_checklist.md)验收，内存调整的实现与第一轮构建证据见[内存修复记录](p4_memory_budget.md)。
