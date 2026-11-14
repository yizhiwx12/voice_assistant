# 日常操作与排查

更新：2026-11-14。当前使用根 `sdkconfig` 与 `build\barge-in-test`，ESP32-S3、ESP-IDF 6.1、QIO／80 MHz。进度和待办只维护在[当前进度](realtime_voice_progress.md)，阶段目标见[总体规划](realtime_voice_plan.md)，帧格式和取消语义见[实时协议](voice_protocol.md)。

首次配置模型使用[中文配置向导](../server/README.md#新手配置)：`.\.venv-voice\Scripts\python.exe server\configure_voice.py`。常用固件设置位于 `Voice Assistant → Basic setup`，VAD/AEC/插话等位于 `Advanced audio and diagnostics`；外设接线统一在 `main/app/board_pins.hpp`。本轮包含固件引脚定义集中与注释整理，现有配置值不变，但仍需由用户构建／烧录验证；仅切换服务端模型通常只需重启 8000。

新增的 [ShowAPI 新闻](../server/README.md#showapi-新闻-api) 只改 Python 服务，无需为它构建或烧录。本机新闻开关默认关闭；先核实官方“不得用于终端展示”限制下的语音播报授权，再通过配置向导“新闻 API 设置”启用，并在密钥菜单填写 `SHOWAPI_APP_KEY`。曼波 9880 保持运行，重启 8000 服务生效。

## 新克隆与默认插话固件

仓库的 `sdkconfig.defaults` 针对 ESP32-S3 N16R8：16 MB QIO Flash、8 MB Octal PSRAM、240 MHz CPU、1000 Hz FreeRTOS，以及本地前端/VAD/AEC/D1/D2 自动插话默认开启。GPIO7/GPIO3 是当前按钮映射，服务器地址 `192.168.1.10` 仅为示例。请核实自己的底板规格与实体接线，然后在 ESP-IDF PowerShell 终端为**首次克隆**执行：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build\barge-in-test set-target esp32s3
idf.py -B build\barge-in-test menuconfig
```

在 `Voice Assistant → Basic setup` 填写 2.4 GHz Wi-Fi、电脑局域网 IP 和实际按键 GPIO；电脑端 TTS/LLM/ASR 选择在 Python 配置向导完成。服务器 IP 示例为 `192.168.1.10`，请换成运行 Python 服务的电脑地址。首次生成配置会采用仓库默认的 D2 插话组合。填写后生成的本机 `sdkconfig` 会包含 Wi-Fi 凭证，不要将该文件或含凭证的配置截图、日志上传到仓库。由用户构建、烧录并在实机验证：

```powershell
idf.py -B build\barge-in-test build
idf.py -B build\barge-in-test -p COM7 -b 115200 flash monitor
```

若需半双工对照，在 `Voice Assistant → Advanced audio and diagnostics` 关闭 `VOICE_AEC_OBSERVATION_ENABLED`；这会同时隐藏/关闭依赖它的 D1、D2 观察和自动插话选项。分别验证两套构建时，给它们使用不同 `SDKCONFIG` 文件和构建目录，不要让两套配置共用同一个缓存。恢复完整 D2 配置后重新构建、烧录。AEC/D2 会增加 PSRAM/DSP 负载并受房间回声和摆位影响；默认开启表示该仓库最终固件配置已选中这些功能，不等于你的麦克风、扬声器和环境已通过验收。

`idf.py set-target` 用于首次配置项目目标，可能重建该 build 目录中的默认配置；对已有工程执行前，先确认本地 `sdkconfig` 中的 Wi-Fi 等设置已另行记录。后续日常构建沿用本操作指南顶部的命令，不要只根据 `build` 或 `build/barge-in-test` 目录名称推断固件功能；应查看实际 `sdkconfig` 中的 `CONFIG_VOICE_BARGE_IN_ENABLED`。

## 构建、烧录与启动

固件构建、烧录、串口监视和服务启动由用户执行。仅整理文档时无需重建固件；修改固件或 `sdkconfig` 后，在 ESP-IDF PowerShell 终端执行：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build\barge-in-test reconfigure
idf.py -B build\barge-in-test build
idf.py -B build\barge-in-test -p COM7 -b 115200 flash monitor
```

字幕功能若在 `main/CMakeLists.txt` 报 `Voice subtitle DMA observer needs the verified IDF 6.1 i2s_channel_write memcpy; SDK changed`，已修正 CMake 将语句末尾分号误计为额外匹配的问题。使用最新脚本重新执行上面的 `reconfigure` 和 `build`；这次修正不需要删除构建目录或修改已安装 SDK。

若 `flash monitor` 在 `voice_assistant_ui.cpp` 报 `no matching function for call to clamp(int32_t&, int, const int&)`，是前置编译失败，尚未进入串口烧录。已将滚动坐标边界计算显式统一为 `int32_t`；保存最新源码后重跑 `build`，成功后再执行 `flash monitor`。这项源码修正无需重新配置或清空构建目录。

`COM7` 不同时替换为实际设备端口。完整 `flash` 会部署同一构建的 bootloader、分区表、应用和模型；首次部署、分区或模型变化时不能只用 `app-flash`。模型清单在 `build\barge-in-test\p4_model_manifest.json`，不要手动混用不同构建目录的文件。

此前完整正文预载与 UI 同时修改 Python 服务和固件，需配套更新；本次曼波切换只改服务端，无需为此重新构建或烧录。当前本地 `realtime_default` 已选择曼波，需要两个 PowerShell 窗口同时运行：窗口 A 按[本地曼波 TTS](../server/README.md#本地曼波-tts)启动 9880 引擎并保持运行；窗口 B 运行下方 8000 服务。仅在窗口 B 已有旧 `voice_server.py` 且需重启时按 Ctrl+C；首次启动不需要 Ctrl+C，窗口 A 也不要停止：

```powershell
cd C:\esp-project\voice_assistant
.\.venv-voice\Scripts\python.exe server\voice_server.py --mode realtime --profile realtime_default --check-config
.\.venv-voice\Scripts\python.exe server\voice_server.py --mode realtime --profile realtime_default
```

首次安装依赖、模型选择、凭证配置、PC 联调和服务端测试见[服务端说明](../server/README.md)。凭证保留在本地配置或环境变量中。

日期直答及 AnySearch 联网查询已接入实时语音服务，只需重启窗口 B。当前本地 DeepSeek 已启用搜索；匿名可用，账号密钥、触发范围、日志及测试问题见[当前日期与联网查询](../server/README.md#当前日期与-anysearch-联网查询)。这次服务端更新不需要构建或烧录。

曼波现在按短段合成，首段完成即下发，播放时预取下一段；仍需先收齐 LLM 回答，无逐词时间戳，键2可手动翻页。当前首段／后续段上限为 40／80 字，调参与验证见[长回答首音等待](../server/README.md#长回答首音等待)。只需在窗口 B 重启 8000 加载此次优化，窗口 A 的曼波继续运行。设备显式请求 `realtime_default`，所以仅更改服务启动的 `--profile` 或 `--tts` 不会覆盖设备选择；切换音色应修改本地 `profiles.realtime_default.tts` 后重启服务。

### 字幕未跟随：此前服务端边界修正

用户实机两轮均出现设备 `subtitle unavailable reason=server_unavailable`，对应服务端 `tts_subtitle_invalid event=350 reason=invalid_schema`。350 是句子开始事件；旧解析器误把边界消息中的字段当成坏字幕，提前禁用了整轮跟随。现已明确跳过 TTS2.0 的句子开始／结束事件 350／351，其他事件的空词列表也继续等待后续时间戳；真正的非空字幕仍严格校验，失败不影响音频。

上述 event=350 修正只改 Python 服务及离线测试，无需为它单独重新构建或烧录固件；本轮完整正文预载还改了 Python 服务，因此烧录新固件后要停止旧进程并启动最新服务。三行自动翻页属于本轮固件修改，仍须按上方命令构建和烧录；有效时间戳驱动下方描述的自动翻页。

服务终端 `subtitle_state status=following` 表示已经向设备发出有效字幕，仍需看屏幕验证同步。若为 `status=unavailable`，保留该行的 `reason/detail` 和同轮身份：`upstream_invalid` 表示上游非空字幕格式／时间无效；`assembler_invalid` 表示文字或时间线对齐失败；`no_cues` 表示本条音频结束仍没有可用字幕。日志不包含回答正文或凭证。

### 字幕成段出现与时序诊断

已确认文字成段出现的直接原因：旧设备在首条 `audio.subtitle` 到达后用该批 TTS 文字替换 LLM 正文，后续 LLM 片段只进入后备缓存；可见内容因此只随字幕批次增长。本轮改成收齐回答、一次提交正文后再播报，时间戳只改变页码，不再缩短或追加回答。

16:00—16:03 的用户日志中，首批字幕分别为 13／15／18 词；首批下发前，发送流程已消费约 1.00／1.58／1.72 秒 PCM 内容。不能把这三个时长当作扬声器已播放时间或字幕实测延迟。提前全文解决文字供给，时间戳晚到仍可能造成翻页追赶，沿用下列诊断区分原因。

另有已复现的本地放大路径：上游音频与字幕共用一个 reader，4 秒 PCM 队列满时等待音频腾位，等待期间不能继续读取后面的字幕。已有的字幕优先出队只帮助已经读到的字幕。两块各 4 秒的离线模拟中，字幕须等首块 4 秒内容消费完才被读到；是否在某轮实机触发，须看新的背压日志。

此前这组 Python 时序诊断保留，本轮不改变音频缓冲策略。当前完整正文和新 UI 需要更新固件并重启服务；若要收集日志，先在旧服务终端按 Ctrl+C，然后执行：

```powershell
cd C:\esp-project\voice_assistant
.\.venv-voice\Scripts\python.exe server\voice_server.py --mode realtime --profile realtime_default
```

让助手完整播报一次较长回答，保留以下三类行。日志经过现有有界后台输出，不逐音频帧打印，不含正文或凭证；没有输出不能直接当作没有等待，先确认进程已重启且没有 `diagnostic_log_dropped`。

| 日志 | 关键字段与含义 |
| --- | --- |
| `tts_subtitle_timing` | `source` 关联同一 TTS 会话；`words` 为该上游事件的词数，`rx_gap_ms` 为有效字幕在本地 reader 的接收间隔，含 reader 自己的等待；`buffered_pcm_ms`、`audio_enqueue_wait_ms/max_ms` 指出音频缓冲与背压。首个间隔用 `-1` 表示无前序事件。 |
| `tts_audio_backpressure` | 音频入队等待达到 20 ms 时输出 `wait_ms/max_ms`，说明共享 reader 确实停止读后续消息；它不是音频断流或设备 DMA 欠载的证明。 |
| `subtitle_timing` | `epoch/turn/generation/stream` 与 `source` 关联设备及上游；`gap_ms` 为 transport 取得含新词字幕的间隔，`provider_queue_ms` 为本地 reader 收到字幕至 transport 处理的等待（含消费、音频 pacing／编码等），`send_ms` 为字幕发送耗时；`sent_content_samples` 仅统计成功发出的有效音频内容，剔除 Opus pre-skip 和尾部补零。`new_words` 及 `start_lead_ms/end_lead_ms` 只看新增词，累计字幕中的旧词不重复计算；负数表示该词边界已被字幕开始发送前的音频发送位置超过，不能直接当作实际播放落后。 |

判断时先看：若 `rx_gap_ms` 已大且有明显 `tts_audio_backpressure`，本地读取背压至少贡献了一部分等待；若 `provider_queue_ms` 大，字幕已经收到但 transport 消费晚；若 `send_ms` 大，检查下行写入／连接背压。`rx_gap_ms` 大且本地等待小仍不足以独立区分云端生成与上游网络；设备 FIFO 与实际显示延迟也需设备日志佐证。不要从 ASR 上行 `queue_wait_ms` 推断 TTS 下行字幕时延。

这些诊断用于定位，未增加音频／字幕协同预缓冲。完整回答已改为播报前到齐，但时间戳仍可能分批晚到；提高刷新频率或拆小发送批次不能补出未知时间戳。

### 屏幕刷新与翻页响应

当前固件路径每轮约 20 ms 检查状态；LVGL 刷新和动画采样周期均为 20 ms，目标调度频率 50 Hz，不等于面板实测 FPS。ST7789 保持 40 MHz，双 DMA 绘图缓冲为 32 行，可将 31 px 中文字幕整行放在一次 SPI 传输中。LVGL 等待 DMA 完成时使用 SPI 完成栅栏，不再忙等；LVGL 接管后会释放启动绘图缓存。`display_perf` 输出刷新周期和耗时，不包括最后 DMA 尾段，不能单独用来宣称实际屏幕帧率。

使用根 `sdkconfig`，在 ESP-IDF PowerShell 终端执行；本项无需单独 `reconfigure`：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build\barge-in-test build
idf.py -B build\barge-in-test -p COM7 -b 115200 flash monitor
```

屏幕／控制器变更需要按上方命令重新构建和烧录。开机日志应为 `ST7789 ready: 240x240, SPI 40000000 Hz`。检查 `display_perf` 的连续刷新平均／最大耗时和提交字节数，并同时听播报是否连续；该行是 LVGL 刷新诊断，不是实测 FPS。40 MHz 的屏幕／接线稳定性须实机确认；若出现花屏或像素异常，将 `main/display/st7789_display.hpp` 中 `pixel_clock_hz` 恢复为 `20'000'000` 后重建烧录，保留其他刷新优化。

### 三行字幕随播报自动翻页

LLM 生成期间的有界文字片段先在设备后台缓存，完整生成后以 `llm.done` 一次提交正文，然后开始播报。开口前要等待整篇 LLM 生成完成，等待期间显示思考状态。正文每页三条完整行，页内保持静止；有效逐词时间戳只定位固定正文，不再用 TTS 分批文字替换回答。按 DMA 实际完成的有效音频样本确定当前读字并翻页，不按固定秒数翻页。多字词内按字数均分词时长，属于近似定位，不表示上游提供逐字时间戳。

深海蓝底、浅青白正文、静态声纹和独立页码构成新的阅读界面，底部区分当前状态与按键提示。保留现有字库、216 px 正文宽度与三行高度，不使用持续动效、阴影或渐变。同页音频进度不触发正文布局和屏幕传输。

迟到 cue 按已完成播放采样追到当前页；播放未推进、取消或结束时保持当前页。没有有效 cue 或 TTS 规范化不能可靠映射原文时保留全文、音频和手动翻页。显示最多 8191 UTF-8 字节，超长回答标记截断，超出可见范围后停止跟随；该容量限制仍保留。全文预载不代表所有词时间戳已提前到齐。

本次同时修改固件与服务，使用根 `sdkconfig`，无需为源码修改单独重新配置：

```powershell
cd C:\esp-project\voice_assistant
idf.py -B build\barge-in-test build
idf.py -B build\barge-in-test -p COM7 -b 115200 flash monitor
```

烧录后停止旧 Python 服务，再用本页上方命令启动新版本。新模式用 `complete_before_audio`／`llm.done` 提交回答，以 `response_word_timestamps` 定位原文；新固件连接旧服务仍保留流式显示，旧固件连接新服务不会启用新模式自动翻页，所以必须配套更新。主机结果以[当前进度](realtime_voice_progress.md)为准，设备验证由用户执行：

1. 请助手回答一个较长问题。思考结束后页数应一次确定；播报开始即可用键2翻阅后文，第一批字幕不能把全文缩回一句。另开一轮不按键，检查三行页内静止、读字进入下一页时切页。
2. 检查中英、数字和标点混排、末页不足三行及尾部换行，均不应截断 UTF-8 或自动翻到空白页。播放缓冲未推进时不自行翻页，晚到时间戳直接追到当前页。数字读法或单位变化导致对齐降级时，全文仍应可读且音频继续。
3. 播报中按键1取消或直接插话，旧字幕和旧声音停止；下一轮从自己的起点开始。键2短按下一页、长按上一页并接管本轮，后续字幕不抢回；下一轮恢复自动翻页。
4. 没有有效字幕时，正文应继续显示且音频和手动阅读仍可用。实际显示耗时、字体像素和声画同步以设备结果为准，离线检查不代替实机结果。

此前离线回归已覆盖元数据失败时正文保留／恢复、播放游标立即追赶及服务端事件背压顺序；三行自动翻页的新增离线结果以[当前进度](realtime_voice_progress.md)登记为准。代理不运行 ESP-IDF 目标构建、烧录、串口、实时服务或设备测试。

## 双按钮与独立操作

当前只使用 `realtime_default` 这条已配置的语音链路，屏幕不再显示组合摘要，也不提供组合详情或选择入口；普通正文和随播字幕均每页三完整行，有效时间戳驱动自动翻页。“独立操作”指无需串口输入，ASR／LLM／TTS 仍需要现有 Wi-Fi 和主机语音服务。

外接按钮接法由用户于 2026-11-13 确认：

| 按钮 | GPIO 与接法 | 短按后松开 | 长按 1 秒 |
| --- | --- | --- | --- |
| 键1 | GPIO7，按下接 GND，内部上拉 | 空闲时暂停／恢复自动监听；录音、识别、思考或播报时取消本轮，随后继续监听 | 清理当前会话，重新尝试连接 |
| 键2 | GPIO3，按下接 GND，内部上拉 | 回答下一页 | 回答上一页 |

GPIO7／3 与当前麦克风 4／5／6、功放 8／15／16／17、屏幕 9–14 没有源码用脚冲突。GPIO3 是 ESP32-S3 绑带脚，上电或复位时先松开；已确认的外接接线无需为这批修改换脚。两键做 30 ms 去抖；长按只产生一次长按事件，不追加短按；开机已按住需先释放，两键同时按不触发操作。有逐词字幕的播报按实际播放进度自动切换三行页面；键2短按下一页、长按上一页并接管手动阅读，手动操作后本轮保持用户选择，下一轮恢复自动翻页。无有效字幕时保留普通回答和手动翻页。

按键改为独立定时扫描，主循环处理较慢时缓存已完成的动作；播报中也支持翻页和键1取消。短按在松开并完成去抖后执行，长按满 1 秒执行一次。只有一页、已经到第一页／最后一页时，对应方向不会继续移动，可看页码判断。屏幕忙时翻页会在后续刷新应用，不清掉待处理动作。

开机等待 Wi-Fi、会话和 AFE 就绪后自动监听，并在当前 D2 配置及健康 AEC 下启用插话，无需输入 `a`／`i`。暂停状态在本次运行的重连后保持；上电重新使用自动监听默认值。D2 不可用时明确显示状态，不能把降级当作插话已开启。

### 自动恢复

- Wi-Fi 每次连接周期最多重试 5 次，整个周期最多 60 秒；失败后显示检查网络／尝试重启。恢复网络后可长按键1重新尝试。
- 语音会话中断后清理旧轮和旧音频，按 1／2／4／8／15 秒退避，最多自动重试 5 次；每次等待 `session.ready` 最多 20 秒。持续就绪 30 秒后重置重试预算，避免反复秒断无限重连。
- 重连成功后恢复用户此前的监听／暂停选择，重新建立 epoch；不补传失败发言、不恢复旧播报。服务端正常待机关闭也自动建立新会话。
- 多次失败后停止自动尝试并显示“请尝试重启”；长按键1可重新开启网络重试预算。音频设备、协议／配置或内存等不可恢复错误不会靠反复联网掩盖，按提示检查或重启。

### 本批验证

已修正播报中漏采按键、屏幕锁无限等待，以及识别准备超时后键1需要按两次的边界。本批改为完整正文先到齐，再按播报自动翻页，并重做阅读 UI；主机检查与目标设备效果分别记录在[当前进度](realtime_voice_progress.md)。使用上方当前命令构建和烧录，同时重启 Python 服务以加载完整正文模式。

1. 不输入任何串口命令，上电后应自行进入自动监听，正常说一句话并得到回答；播报中能按既有条件正常插话。
2. 空闲时短按键1暂停，再短按恢复；播报或识别中短按键1取消，旧声音不回来，下一句可以继续。两键长按松开后不得再额外触发短按。
3. 按上方[三行字幕验证](#三行字幕随播报自动翻页)检查全文预载、页内静止、跨页切换与停播冻结，再检查键2手动阅读接管和下一轮恢复自动翻页。无有效时间戳时应能继续听音频并手动阅读完整回答。
4. 临时停止再启动主机服务，或断开再恢复 Wi-Fi，确认显示重连且成功后回到之前模式；耗尽后提示重启，长按键1能重新尝试。先前问题不能被重新上传或重播。
5. 合并完成下方起音检查：短促动静不启动空识别，正常短句首字完整。

### 可选串口调试

根配置 `VOICE_SERIAL_COMMANDS_ENABLED=n`，正常使用不安装 UART0 输入驱动；串口日志仍可查看。确需定位问题时在 menuconfig 打开该选项，重建后使用原 `?` 诊断、`a/i/m` 模式、`r/s/c` 发言和取消、`n` 重连及 `[`／`]` 翻页。`v` 已移除。串口调试不是设备日常操作步骤；实际服务组合仍记录在接受 `session.ready` 后的 `session_selection` 日志中。

## 当前自动监听与插话参数

空闲起音与播报期间插话分别确认；下表为当前根配置，参数单位和含义不要混用。

| 场景／参数 | 当前值及条件 |
| --- | --- |
| 空闲起音 `VOICE_AUTO_START_CONFIRM_MS` | 160 ms，即连续 8 个 20 ms VAD 人声帧 |
| 空闲能量 `VOICE_AUTO_START_MIN_AC_RMS` | 整窗 AC RMS ≥80，至少 4／8 帧达标，最后 3 帧至少 2 帧达标 |
| 播报插话 `VOICE_BARGE_IN_CONFIRM_MS` | 200 ms，即连续 10 个 20 ms VAD 人声帧 |
| 插话能量 `VOICE_BARGE_IN_MIN_AC_RMS` | 整窗 AC RMS ≥80，至少 5／10 帧达标，最后 3 帧至少 2 帧达标 |
| 预录 `VOICE_AUTO_PREROLL_MS` | 400 ms，确认音频追加在预录后，完整交接 |
| 重新就绪／尾部静音 | 起音前需要 300 ms 连续静音；采集后按 `VOICE_AUTO_SILENCE_MS=700` ms 静音提交 |
| 握手 `VOICE_AUTO_HANDSHAKE_MS` | 2000 ms；空闲起音在本地确认后才开始计时，D2 取消确认和新流开启共用此预算 |

AC RMS 是去均值后的能量统计，不是音量百分比。已确认的 D2 交接绕过空闲起音门，不重复等待 160 ms。手动录音不使用这些起音确认条件。

## 验证最新起音过滤

160 ms 空闲起音门已通过主机回归，目标构建、烧录和设备效果仍待用户验证。实现与测试证据见[起音修正记录](archive/p4/p4_idle_onset_filter_20261112.md)。

1. 等待自动监听就绪；若已暂停，短按键1恢复，然后安静片刻。
2. 制造原先会起空识别轮的短促动静，预期保持监听，不建立云端识别回合。
3. 用平时音量说一句完整短句，预期正常识别且首字完整。
4. 若仍异常，先记录屏幕状态和发生阶段；需要深入诊断时启用上方可选串口调试，保留 `auto_start`、相关识别结果和首因错误日志。

`auto_start` 的 `candidates` 是本地候选数，`confirmations` 才是空闲起音确认数；`rejected` 表示 VAD 中断，`energy_rejections` 表示能量或持续性不足。计数按控制器配置以来累计，比较同一运行中的前后变化；D2 交接不计入空闲确认数。

此保护用于拦截瞬态及其 VAD 保持。持续高能噪声仍可能通过；极短或过弱的话可能被拒绝。拒绝后需要重新静音就绪，再说一句，不依据一次快照盲目提高阈值。

## 日志在哪里看

设备日志在串口监视终端；启用可选串口调试后，`?` 输出一组诊断。服务端日志在运行 Python 服务的终端。按下表搜索行名前缀和字段，保留同一 `epoch/turn/generation` 的上下文。

| 日志行名前缀 | 重点字段与解释 |
| --- | --- |
| `conversation: session_selection` | `epoch`、`metadata_present`、`rejected_fields`、profile、服务模式、交互模式及连接状态；三层后缀 `_asr/_llm/_tts` 给出实际公开名称和三态能力 |
| `conversation: local_frontend` | `ready=1 aec_active=1 error=0` 表示前端／AEC 正在运行 |
| `conversation: auto_start` | `confirm_ms`、`min_ac_rms`、候选／确认／拒绝数、`window_ac_rms` 和活动帧数 |
| `conversation: barge_in_observation` | `enabled`、`phase`、`reason`、`mode`；区分关闭、观察和实际打断 |
| `conversation: barge_in_energy` | 候选／确认事件的 AC 能量及 `energy_active_frames`、`energy_tail_active_frames` |
| `conversation: barge_in_handoff` | `confirmed` → `local_fence` → `cancel_ack` → `speech_start` → `first_uplink` |
| `audio_service: reference_alignment` | `healthy=1 unmapped=0`；参考数字时钟与映射健康 |
| `audio_service: aec_reference_progress` | `timeline_healthy=1`、`last_query_valid`、缺失区间及 `unmapped` |
| `audio_service: capture_timeline` | `valid=1 overflow=0 invalid=0`；采集时线和 DMA 溢出 |
| `audio_service: aec_pairing` | `active=1 error=0`、递增的 `pairs`、`last_reference_valid`、`frontend_waits` |
| `audio_service: playback_service` | `content_write_gap_max_us`、`empty_waits`；这些不等于实测 DMA underrun |
| `max98357a: playback_reference` | `queue_dropped`、`discontinuity`、`invalid`；检查新增丢块和边界异常 |
| `memory` / `audio_service: audio_task_stacks` | 内部 RAM、最大连续块、`alloc_failures`、任务最低剩余栈，关注趋势 |
| 服务端 `voice_asr_result` | `outcome`、`final_chars`、`pcm_ac_rms`；区分真实空最终结果与失败 |
| 服务端 `voice_asr_summary` | 当前回合媒体落后、队列等待、接收空档及 worker 阶段 |
| 服务端 `subtitle_state` | `status=following/unavailable`、`reason/detail`、同轮身份及字幕批次／词数；区分已下发字幕与上游／组装失败或无时间戳 |

数字参考健康、配对成功和 `aec_active=1` 证明处理链在运行，不能证明回声抑制效果、人声保真或校准完成。`calibrated=0` 仍表示未校准；标记 `input_output_aligned=0` 的 RMS 不能直接相除估算回声消除量。

## 常见现象

| 现象 | 先检查 |
| --- | --- |
| 插话观察不可用 | `barge_in_observation reason`、连接状态、`local_frontend ready/aec_active/error`；当前配置会在就绪后自动启用 D2 |
| 短声仍起空识别轮 | 对比 `auto_start` 确认数是否增加，核对本批启动日志 `Automatic onset: confirm=160 ms min_ac_rms=80` |
| 偶发 `AFE Ringbuffer ... empty` | 联看输出是否持续、队列、错误和时线；单条等待警告不足以认定 AEC 失效 |
| ROM 先显示 `mode:DIO` | 看随后 bootloader 的 `SPI Mode : QIO` 与应用 `flash io: qio`；QIO 镜像先以 DIO 加载是正常流程 |
| 模型缺失／校验失败 | 核对同一构建的完整 `flash` 与模型清单，保留首次错误；不要混烧旧模型 |

逐批故障分析和旧实验参数查[历史记录](archive/README.md)。当前问题的处理顺序和延期事项以[当前进度](realtime_voice_progress.md)为准。
