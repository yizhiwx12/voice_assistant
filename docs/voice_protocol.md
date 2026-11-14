# 实时语音设备协议 v1

服务端、PC 测试客户端及 ESP32-S3 固件均按此协议实现；最新验证状态见[当前进度](realtime_voice_progress.md)，运行方法见[操作指南](operations.md)。入口为 `GET /ws/voice`，WebSocket 文本帧为 UTF-8 JSON，二进制帧为 32 字节头加一个音频包。服务端可同时接入 4 个设备连接。

## 建立连接

客户端先发：

```json
{"type":"session.start","version":1,"profile":"realtime_default","uplink_codec":"opus","downlink_codec":"opus"}
```

`profile`、`asr`、`llm`、`tts` 可省略，继承服务启动时选项；显式指定 profile 时，未覆盖的三项继承该 profile。服务端校验配置及凭证后返回 `session.ready`，包括新的 `session_epoch`、当前组合和能力、上行 16000 Hz/下行 24000 Hz、20 ms 帧长、协商的编码。

`selection` 是当前会话的公开描述，含 `profile`、`mode`、`require_streaming_asr` 及 `asr/llm/tts` 对象；每层含 `id`、`adapter`、`capabilities`，可选 `model/voice/resource_id`。能力中的 `streaming_input/streaming_output/partial_results` 是布尔值。该描述不含凭证、端点或任意 provider options；表示配置能力，不等于云权限或声学效果验证。

设备内部组合诊断只接受上述白名单的有界单行 UTF-8 文本：profile／id 最多 80 字节、adapter 32、model／voice／resource_id 96、mode 16；超限／错误类型字段置为未提供，不截成另一个标识。缺失能力为未知，不能当作 false 或支持；未知扩展字段不消费。元数据缺失或无效不阻断支持的音频握手，诊断用 `metadata_present/rejected_fields` 区分。设备快照仅绑定被接受的 epoch，在故障、关闭和重连时失效；新握手才确认新组合。设备目前仍显式发送编译期 profile，服务启动时各层 CLI 覆盖不会自动替换该 profile。

两种编码均为单声道。默认 raw Opus；显式请求 `pcm_s16le` 且配置允许回退时才采用 PCM。Opus 库不可用时明确失败。一个连接期间不能改变协商的编码；切组合或重连需要新连接和新 epoch，旧回答不恢复。

## 上行与发言

1. `audio.stream.start`：携带 epoch、唯一 `stream_id`、codec、sample_rate_hz=16000、channels=1、frame_duration_ms=20、pre_skip_samples。PCM 的 pre-skip 必须为 0；Opus 使用该编码器实际前瞻样本数。每个连接最多建立 1000 个输入流，流号不能重复。
2. `speech.start`：携带 epoch 和递增的 `turn_id`，服务端在云 ASR 就绪后回应 `speech.started`。客户端应等待此确认再发送当前发言的预录音频，避免云握手期间填满有界输入队列。P3 手动模式在就绪后提示讲话；P4.2 自动模式由本地 VAD 提出起止候选，并使用有界预录及握手缓冲。PC 工具根据完整 WAV 显式标记边界。
3. 发送 20 ms 音频包：Opus 每包一个 raw packet，PCM 每包 320 样本/640 字节。序列号从 0 开始，连续递增；sample_offset 从 0 开始，每包加 320。**持续上行包的 turn_id 和 generation 均为 0**，流状态独立于回答。
4. 有限输入流需排空编码前瞻（即使 PCM 长度是 20 ms 的整数倍），再发 `audio.stream.end`，携带 stream_id 和 `valid_end_sample=pre_skip_samples+原始有效PCM样本数`。末包可以补零，接收端用有效结束位置裁剪。
5. `speech.end` 携带 epoch 和当前 turn_id。有限流先结束音频流，再结束发言。若持续采集流保持打开，需另带 `sample_offset`：此轮有效结束的解码样本位置；要等后续静音音频包已携带该位置所需的 Opus 前瞻，再发此事件。服务端保留最后一帧用于裁剪。一个发言边界最多等待一帧；不能引用尚未收到或已经输出之前的位置。

识别期间返回 `asr.partial`、`asr.segment_final`。只有 `asr.utterance_final` 提交整轮问题并生成一次回答。重复的同轮 speech.end 不重复调用模型；旧轮次的迟到结束事件被忽略。新 speech.start 会取消旧回复；上一个 ASR 尚在等待结果时，新发言会取消其最终确认任务，再建立新识别器。

本地 VAD 静音端点、手动 `s` 和最长录音保护均使用同一 `speech.end`，不分别调用模型。尚未检测到讲话的自动监听无需建立 ASR 回合，也不因监听满 30 秒自动提交空问题。录音中的静音如何发送由当前 ASR 适配器管理，不能在仍打开的云输入中任意丢弃音频。

发言最长约 30 秒，含 Opus 尾部余量。上行工作队列最多 25 项（约 500 ms 音频，控制事件也占项）；短暂满队列允许等待最多 100 ms，并让消费任务运行。入队等待超限，或入队/消费/解码后检查发现音频排队年龄、相对媒体落后量达到 750 ms 时，以 `uplink_backlog` 明确报错并关闭连接，不扩大容量。相对媒体落后量以本轮首次接收的样本偏移和服务端时间为原点，能够发现持续慢消费将积压转移到网络缓冲的情况；不测量首帧之前的固定延迟，也不是端到端绝对时延保证。正在等待的 provider 调用仍由其独立超时管理。客户端应恢复到新会话，不能丢若干 Opus 包后继续。

## 下行与取消

录音或 ASR 握手／收尾期间，客户端可发送 `speech.cancel`，携带 epoch 和目标 `turn_id`。服务端先封禁该轮，再取消正在等待的输入操作及最终识别任务，丢弃队列和输入流，关闭识别器，并返回带目标轮次的 `speech.cancelled`。取消不调用 ASR 结束识别接口，不将部分文本或空结果提交到 LLM／TTS；若该轮回复已开始，则同时停止该轮回复。设备应先停止本地采集上行并封禁该轮，再发送取消，收到确认后才开始下一轮。

同轮重复取消幂等；旧轮的迟到取消可单独确认，但不触碰新轮。被取消的输入流号永久作废，其迟到的开始／音频／结束事件被丢弃；下一轮必须递增 turn 并使用新 stream。发送尚未宣布的未来轮次取消是协议错误。此路径保留当前 WebSocket 连接，不替代下述已知回复三元组的 `response.cancel`。

取消清理预算为 5 秒；超时保留真实故障并关闭会话，不发送成功取消确认，不复用未确认清理的识别／编解码状态。

回答事件带 `(session_epoch,turn_id,generation)`。当前 `response.started` 带 `text_mode="complete_before_audio"`：LLM 生成期间仍通过每条最多 1024 UTF-8 字节的 `llm.delta` 交付后台片段，整篇完成后发不带正文的 `llm.done`。设备缓存这些片段，在 `llm.done` 时一次提交正文，之后服务端才启动 TTS 和下行音频。整篇回复因此从播报开始前即可翻阅，不随字幕批次缩短或追加；代价是首音需等待整篇 LLM 生成完成。`response.started` 表示开始生成，`response.done` 表示云端生成/合成及服务端下发已完成，**不表示设备已经播完**。旧服务未提供 `text_mode` 时，新固件保留原增量文字行为。

每条下行回复独立发 `audio.stream.start`，含 codec、24000 Hz、单声道、20 ms、pre_skip_samples 和独立 stream_id。二进制头标识该回复，每帧 480 解码样本，PCM 960 字节；Opus 包长可变。最后发 `audio.stream.end`，携带有效解码结束位置。设备需丢弃开头 pre-skip，保留最后一帧直到 end，再裁掉补零及无效尾部。

### 播报字幕扩展

当前完整正文路径在支持逐词时间戳时，`audio.stream.start` 带 `subtitle_mode="response_word_timestamps"`。字幕绑定同一 `(session_epoch,turn_id,generation,stream_id)`，保留 TTS 的原始样本时间，将词位置顺序映射到已提交的固定回答正文。`audio.subtitle` 的 `text` 恒为空、`text_offset` 恒为 0，只补时间元数据；`words[].text` 为正文对应切片，`text_byte_offset` 为该切片在完整回答中的绝对 UTF-8 字节位置。设备校验切片和边界，正文不再由字幕事件写入。

对齐可忽略标点、空白和简单 Markdown 标记，并容许 Unicode NFKC／大小写差异；数字读法、单位展开或实质文字不同而不能可靠对齐时，停止自动跟随并保留全文与手动翻页，不按字数或总时长猜测位置。原来的 `subtitle_mode="word_timestamps"` 仍兼容旧服务，它使用下方的分批字幕正文格式；无字幕时为 `none`，省略时也按无字幕处理。旧固件不识别新 mode，仍能接收 `llm.delta` 全文，但不会自动翻页，需配套更新。

当前固定正文示例（正文已在 `llm.done` 时提交为“你好。世界。”）：

```json
{"type":"audio.subtitle","session_epoch":1,"turn_id":2,"generation":3,"stream_id":4,"sequence":0,"text_offset":0,"text":"","words":[{"text":"你好","start_sample":0,"end_sample":12000,"text_byte_offset":0}],"covered_sample":12000}
```

以下为兼容的旧字幕正文模式：

```json
{"type":"audio.subtitle","session_epoch":1,"turn_id":2,"generation":3,"stream_id":4,"sequence":0,"text_offset":0,"text":"你好。","words":[{"text":"你好","start_sample":0,"end_sample":12000,"text_byte_offset":0}],"covered_sample":12000}
```

`sequence` 从 0 连续递增；`text_offset` 是本批新增 UTF-8 正文的起始字节位置，`text_byte_offset` 是词在整条字幕正文中的绝对 UTF-8 字节位置。`start_sample/end_sample` 以本条 TTS 的有效 24 kHz PCM 为原点，不包含 Opus pre-skip、补零和设备 underrun／drain 静音。上游累计字幕可能先给正文、稍后补词时间戳，因此 `text` 可以为空，词位置也可以落在先前已显示的正文中；新增词的位置和开始时间不能倒退。重复批次忽略，缺批、非法 UTF-8／边界或超限会停止本轮自动跟随。

服务端每批最多 24 个词，旧模式每批最多 1024 字节新增正文。设备正文最多保留 8191 UTF-8 字节前缀，超过时显示截断提示；时间戳超出可见正文则停止跟随，健康音频仍继续，不能把前缀称作任意长度的完整回复。设备保留最多 512 个尚未消费的词锚点，并限制时间戳在 180 秒内。设备用实际 DMA 播放完成的有效样本数消费锚点，LVGL 按当前字体和换行显示三行，在读到下一页的字时自动翻页；词内按字数均分时长近似定位。网络暂缓或播放停顿时字幕不按墙钟前进。手动翻页接管本轮，下一条回复恢复跟随。

上游未提供或无法继续校验字幕时，服务端发 `audio.subtitle.unavailable`，携带同一身份及 stream_id；新增诊断字段 `reason` 区分 `upstream_invalid`、`assembler_invalid` 和 `no_cues`，`detail` 为固定的内部错误枚举。旧设备忽略这些额外字段，仍按同一身份退出跟随。字幕故障不打断健康音频。设备保留普通 LLM 回答供翻页；取消、断线和新回合清除旧字幕身份，迟到字幕不能推动新回复。`response.done` 和 `audio.stream.end` 不代表实际播完，设备仍按实际播放时钟跟随到完成。

火山 TTS2.0 的句子开始／结束事件 350／351 不参与字幕解码，逐词数据从独立字幕事件接收；其他事件的空 `words` 列表表示此消息没有逐词时间戳，不提前关闭字幕。直到流结束仍无有效字幕才通知 `no_cues`。首次有效批次和故障原因记在服务端 `subtitle_state` 日志；该日志表示下发状态，不代表屏幕同步已经验收。

### 取消与完成确认

客户端打断时先在本地封禁旧回复身份、停播并清理播放队列和 DMA，然后发 `response.cancel`，携带目标三元组。服务端取消旧模型任务、关闭对应 TTS 会话并返回幂等的 `response.cancelled`。确认前后到达的旧包都不能重新解码或入播放队列。普通回复取消不重建持续上行编解码器。

每条匹配当前回复的取消请求只产生一次确认；对同一回复的重复请求仍可分别确认。客户端按三元组处理确认，不能将迟到旧确认当成新轮结束。

实际播放完毕才可发 `playback.done` 三元组。没有文本与音频对齐时，服务端不会据播放时长推算“已经听到哪些字”；被打断的历史明确标记。PC 工具只保存 WAV，所以不会发送播放确认。

## 二进制头

全部字段为小端，固定 32 字节：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| version | u8 | 1 |
| kind | u8 | 1=上行，2=下行 |
| header_bytes | u16 | 32 |
| session_epoch | u32 | 当前连接返回的 epoch |
| stream_id | u32 | 独立音频流编号 |
| turn_id | u32 | 上行持续流为 0，下行是回答轮次 |
| generation | u32 | 上行为 0，下行为回复代数 |
| sequence | u32 | 每条流从 0 开始 |
| sample_offset | u64 | 包解码后起始样本位置，包含编码前瞻时间线 |

音频 payload 为 raw Opus packet 或 PCM16LE，不带 WAV/Ogg/WebM 容器。每个 packet 恰好 20 ms；错误 epoch、方向、序号、偏移或包时长均拒绝。

## 错误与生命周期

错误 JSON 使用固定 `code`，不包含厂商正文、音频或凭证。TTS 失败时尽量继续返回完整回答文本，response.done.reason 为 tts_failed；设备应显示语音失败。ASR 失败、协议错误、慢连接或断线会释放识别/合成任务。断线重连后必须清除旧回复及编解码状态。

输入控制消息最多 8 KB；首次握手等待 15 秒，WS 心跳 20 秒。`transport.idle_timeout_seconds` 为 0 时保持待机连接（默认），取 1–86400 时启用有限待机。录音、ASR 收尾、回复生成和下行播放保护期间不计为待机；各云端操作保留独立超时。没有业务音频时无需为保活持续上传静音。

豆包 ASR 允许录音期间不产生中间识别结果。适配器按成功发送的非空 PCM 音频刷新录音进度期限；持续正常输入不因缺少识别响应中断，空输入、仅在本地缓冲或远端心跳不算发送进度。结束包发送成功后，最终结果拥有完整独立操作预算；只有真实空 final 才返回 `asr.utterance_final` 空文本及 `response.done / empty_utterance`，设备恢复待录音并保留会话。上游错误、关闭、输入停滞或最终结果超时仍是故障，不转换为成功。

有限待机到期时，服务端发送 `{"type":"session.closed","reason":"idle_timeout",...会话标识}`，随后以正常关闭码 1000 结束连接；该事件不属于 `error`。设备清理旧会话后自动建立新会话，恢复此前的监听／暂停选择；不会因重连本身创建新录音或重放旧问题。Ping/Pong 只检测连接存活，不刷新业务空闲时间。

播放结束以匹配当前回复的 `playback.done` 为准。未发送确认的 PC 保存工具使用有限的音频时长保护，保护到期只允许进入待机，不会据此标记用户已听完。真实协议、上游和持续积压故障仍使用固定错误 code 和异常关闭，设备保留首因并按有界退避自动重连；耗尽后提示尝试重启，长按键1可手动重开网络重试预算。不可恢复的设备或配置错误不循环重试。
