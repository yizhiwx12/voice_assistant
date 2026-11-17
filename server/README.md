# 语音助手服务端

`voice_server.py` 提供两种运行方式：兼容的 HTTP 整段识别，以及 `/ws/voice` 双向实时语音。ASR、LLM、TTS 可分别选择。默认仍为 Qwen 整句识别 → DeepSeek 的 HTTP 模式。

当前本地 `realtime_default` 组合为豆包 ASR 2.0 → DeepSeek SSE → 曼波 TTS，通过 `--mode realtime --profile realtime_default` 选择，先启动[本地曼波引擎](#本地曼波-tts)。配置模板仍保留火山 TTS 2.0（Vivi 2.0）组合，其 PC 整链路、同连接多轮、首音频后取消恢复及 PCM/Opus 小样本云验证见 [P2 云端验收](../docs/archive/p2/p2_cloud_acceptance.md)。用户于 2026-11-17 确认当前曼波设备链路已实测正常，项目按当前功能范围收尾。

项目入口见[文档索引](../docs/README.md)，结项状态与已知限制见[当前进度](../docs/realtime_voice_progress.md)，设备构建／烧录／串口操作见[操作指南](../docs/operations.md)，架构与阶段目标见[总体规划](../docs/realtime_voice_plan.md)。本文维护服务端环境、配置、PC 工具和诊断说明。

用户已确认自动监听、160 ms 起音过滤、三行字幕、双按钮、D2 插话、重连、多轮、曼波连续播报、AnySearch 和新闻搜索均正常。曼波中文播报与音色连贯性正常；未知英文词可能逐字母读出。用户观察网络良好时首音通常在 1 秒内，网络较差时可能为 2–3 秒，未提供统一计时起点或样本统计，不能作为延迟承诺。声学主要验证了近端讲话和正常插话，未宣称完整远端／双讲／跨距离校准或长期资源统计通过。历史批次的“未执行”“待验”只描述当时证据；当前验收以这次用户反馈为准。

设备帧格式、回合边界和取消语义见 [实时协议 v1](../docs/voice_protocol.md)。

| 层 | 已实现的适配器 | 当前能力 |
| --- | --- | --- |
| ASR | Qwen `qwen_batch`、豆包 2.0 `doubao_streaming` | Qwen 收齐一轮后识别；豆包上传 PCM 并接收 partial/确定片段 |
| LLM | DeepSeek / Qwen 的 `chat_completions` | HTTP 回答及 SSE 文本增量；DeepSeek 实时默认组合已验证，Qwen 配置待验证 |
| TTS | 火山 `volcengine_bidirectional`、阿里 `cosyvoice_bidirectional` | 双向流式文本与 PCM24k 音频；音色/模型需匹配账号 |
| TTS | 曼波 `gpt_sovits` | 旧版 HTTP `POST /` 分段合成、预取下一段；WAV 转单声道 PCM24k，提供分段级播放 cue（非逐字对齐） |

目录按职责划分：`core/` 管理配置和回合，`providers/` 封装云协议，`transport/` 处理 HTTP/WS，`audio/` 处理 PCM/Opus，`config/` 保存配置示例及本地凭证，`data/` 保存调试结果，`tests/` 保存离线测试。

## 环境与依赖

语音服务和配置向导要求 **Python 3.11 或更高版本**，推荐使用本项目当前已验证的 **Python 3.12（64 位）**。实时流程使用 Python 3.11 引入的 `asyncio.timeout`；即使 Python 3.10 能安装部分依赖，也不能运行本项目的这两个入口，启动时会先给出中文版本提示并退出，不读取配置或凭证。

以下命令在项目根目录运行。Windows 先安装 Python 3.12 和 Python 启动器，再明确指定版本创建独立的 `.venv-voice`：

```powershell
py -3.12 --version
py -3.12 -m venv .venv-voice
.\.venv-voice\Scripts\python.exe --version
.\.venv-voice\Scripts\python.exe -m pip install -r server/requirements.txt
.\.venv-voice\Scripts\python.exe server/voice_server.py --list-options
```

若没有 `py` 命令，也可先运行 `python --version`，确认是 3.11+ 后再用该解释器创建虚拟环境。已有 `.venv-voice` 时，先检查其中 `python.exe --version`；安装新版系统 Python 不会升级旧虚拟环境，需要用新版解释器重新创建它并安装依赖。曼波引擎自带的 `runtime/python.exe` 使用上游指定版本，不按本节替换。

依赖锁定为 `aiohttp==3.14.5`；Windows x64 的 `PyOgg==0.6.14a1` 提供 `opus.dll`，项目直接通过 libopus C ABI 编解码。当前 DLL 的版本字符串为 `libopus unknown`，复现信息及替换方法见[原生 Opus 依赖](#原生-opus-依赖)。

### 原生 Opus 依赖

项目通过 `ctypes` 调用 libopus C ABI，使用 raw Opus packet，并未采用 Ogg/WebM 容器。Windows AMD64 的原生库由上述 PyOgg wheel 提供。

当前 Windows DLL 能加载且完成编解码测试，但 `opus_get_version_string()` 返回 **`libopus unknown`**，无法据此确认 libopus 上游发行版本。本机已检查的 DLL SHA-256 为：

```text
5fa25b62f72f880acf420120d8ca32d31997620cadaab26ae798d13355012d1b
```

wheel 版本与文件哈希便于复现，不替代上游版本来源证明。若需要明确的 libopus 发行版本，可配置 `OPUS_LIBRARY` 为已核对的原生库绝对路径，并重新跑编解码测试及配置检查。加载顺序是显式 `OPUS_LIBRARY` → 系统查找 → Windows PyOgg DLL；Python 与 DLL 架构须一致。

Linux 使用系统 libopus，例如 Debian/Ubuntu。先安装发行版提供的 Python 3.12 和对应 venv 包；下面显式使用 `python3.12`，不要直接假定系统 `python3` 已满足最低版本：

```bash
sudo apt-get install libopus0
python3.12 --version
python3.12 -m venv .venv-voice
.venv-voice/bin/python --version
.venv-voice/bin/python -m pip install -r server/requirements.txt
.venv-voice/bin/python server/voice_server.py --list-options
```

发行版没有 Python 3.12 时，可使用已安装的 `python3.11` 替换上面的 `python3.12`；先确认版本输出至少为 3.11。Linux 尚未实际运行验证。缺失原生库时明确报错，WS 不在当前流中静默切回 PCM；调试时由客户端显式请求 `pcm_s16le`，且服务配置必须允许回退。

## 配置与启动

### 新手配置

先理解三个名字：**ASR 听懂你说的话，LLM 想出回答，TTS 把回答读出来**。三层可以独立选择；切换聊天模型通常不需要重新烧录设备。

推荐使用中文菜单，在项目根目录打开 PowerShell：

```powershell
cd C:\esp-project\voice_assistant
.\.venv-voice\Scripts\python.exe server\configure_voice.py
```

也可用已有 PowerShell 脚本进入相同菜单（仅此次进程使用指定执行策略）：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\server\start_voice_server.ps1 -Configure
```

1. 菜单 1、2、3 分别选择识别、聊天、播报条目。默认编辑 `profiles.realtime_default`，就是当前设备请求的组合；没有本地配置时从模板开始，首次保存才创建文件。
2. 菜单 4 修改模型 ID、接口地址、音色、曼波参考音频、语速等参数。千问/CosyVoice 模板中的 `<...>` 必须先换为自己账号和区域支持的真实值，再选择该条目。按回车保留当前值，`/default` 删除可选参数；必填参数不能删除。
3. 菜单 5 填写当前组合所需凭证，输入不回显，已有其他凭证保留。菜单 6 检查已保存配置、必需凭证和本机 Opus。检查不调用云 API，也不保证模型权限、9880 引擎或网络已正常。
4. 保存后停止旧的 **8000 语音服务**，再按下方命令启动；使用曼波时 **9880 引擎窗口保持运行**。按回车退出向导。

```powershell
# 只查看：不写文件、不读密钥
.\.venv-voice\Scripts\python.exe server\configure_voice.py --show
# 只做离线检查
.\.venv-voice\Scripts\python.exe server\configure_voice.py --check
# 检查通过后由用户启动实际服务
.\.venv-voice\Scripts\python.exe server\voice_server.py --mode realtime --profile realtime_default
```

**文件和生效顺序**：

| 文件/设置 | 用途 | 新手需要做什么 |
| --- | --- | --- |
| `server/config/voice_config.json` | 本机模型参数与组合 | 日常交给向导编辑；已有文件不要拿模板覆盖 |
| `server/config/server_secrets.json` | API Key、Workspace 等凭证 | 通过菜单 5 填写，或手动编辑；不用的服务可以不填 |
| `server/config/voice_config.example.json` | 不含密钥的模板 | 本地业务配置不存在时使用，不是当前本机配置 |
| `server/config/backups/` | 每次覆盖保存前的原文件 | 恢复时取对应文件覆盖本地文件；凭证备份也含密钥，整个目录不提交或分享 |
| 环境变量，如 `DEEPSEEK_API_KEY` | 覆盖文件中同名凭证 | 优先级最高；显式空值也会覆盖，请检查启动服务的窗口 |
| 旧 `model_ref`，如 `DEEPSEEK_MODEL` | 从环境变量/凭证文件覆盖模型名 | 向导直接修改 `model` 时会移除此条目的 `model_ref`，让新值生效；手动编辑时也要核对 |

保存采用同目录临时文件替换，已有文件先备份；向导发现文件被其他窗口改动时拒绝覆盖，请退出后重开。原有 JSON 业务字段和未使用的凭证条目都会保留，JSON 缩进统一为两空格。整个菜单不启动服务，也不调用云模型。

**当前支持的选择**（ID 是配置条目名，不是厂商模型 ID）：

| 层 | 可选条目 ID | 使用条件 |
| --- | --- | --- |
| ASR | `doubao_2` | 豆包流式识别；账号需开通相应 `resource_id` |
| ASR | `qwen_existing` | 整句结束后识别；向导同步关闭该组合的 `require_streaming_asr`，不会产生实时中间文字 |
| LLM | `deepseek_default` | 当前 DeepSeek 聊天接口；`model` 以账号实际可用模型为准 |
| LLM | `qwen_default` | 先填写模板中模型 ID 和区域接口地址；兼容性与账号权限需验证 |
| TTS | `mambo_local` | 先启动本地 9880 引擎，参考路径是引擎电脑上的文件 |
| TTS | `volc_default` | 火山在线音色，需要相应 Key 和音色权限 |
| TTS | `cosyvoice_default` | 先填写模型、音色和区域接口地址；账号权限需验证 |

例如将现有设备从曼波改为火山：运行向导 → 菜单 3 → 选 `volc_default` → 菜单 5 填凭证 → 菜单 6 检查 → 重启 8000 服务。ASR 和 LLM 保持原选择，无需烧录。

**手动编辑时看这些字段**：JSON 不支持注释，使用英文双引号，布尔值写 `true`/`false`，最后一项后不要加逗号。Windows 路径可以写 `C:/folder/file.wav`，使用反斜杠时要写成 `C:\\folder\\file.wav`。

| 字段路径 | 含义和调法 |
| --- | --- |
| `profiles.realtime_default.asr / llm / tts` | 填上表中的条目 ID，选择当前设备组合 |
| `llm.<条目>.model` | 厂商控制台的模型 ID，不是 `deepseek_default` 这样的本地别名 |
| `llm.<条目>.base_url` | 聊天服务基础地址，适配器自动加 `/chat/completions`；如果同时设置 `endpoint`，后者优先 |
| `llm.<条目>.max_tokens` | 回答长度预算；实时缺省 1024。调大可能增加等待和阅读页数，账号模型还可能有更低上限 |
| `llm.<条目>.temperature` | 随机程度 0–2；并非所有兼容模型都接受全部采样参数 |
| `llm.<条目>.enable_web_search` | `true` 允许新闻等问题触发 AnySearch；`false` 关闭；不是每个问题都搜索 |
| `tts.volc_default.voice` | 火山音色 ID；改变音色前先确认账号支持 |
| `tts.mambo_local.speed` | 语速，1 为正常；支持 0.1–4，仍受引擎能力限制 |
| `tts.mambo_local.first_segment_chars / segment_chars` | 首段/后段字符上限，缺省 40/80，支持 8–200；太短可能增加合成请求与停顿 |
| `<层>.<条目>.timeout_s` | 单个 provider 超时秒数；外层整轮仍有 120 秒预算，单独调大此值不能延长整轮 |
| `transport.*` | 音频协议参数，建议保留；采样率/帧长必须与设备匹配 |

### ShowAPI 新闻 API

项目可选接入 ShowAPI 新闻 109，用 `109-34` 列出频道、用 `109-35/109-36` 查新闻、用 `109-37` 取热搜。它与 LLM 的 `enable_web_search` 是独立开关：打开后，明确的新闻播报请求走新闻频道；具体事件“真假/怎么回事”仍走原 AnySearch 搜索；日期和普通闲聊保持原路径。

**授权状态待确认。** 官方 [OpenAPI 说明](https://www.showapi.com/openapi/market/109.json) 明确写明“仅供内部数据分析、统计及机器学习使用，不得用于终端展示”，并链接到[平台服务条款](https://www.showapi.com/helpcenter/view#/4019/2)。用户已于 2026-11-17 确认新闻搜索与播报技术上可用；这不证明账号授权已覆盖终端语音播报，授权范围仍需向 ShowAPI 核实。项目模板默认 `news.enabled=false`，模板值不代表用户当前本机设置。

确认获准后，在项目根目录运行中文配置向导：

```powershell
cd C:\esp-project\voice_assistant
.\.venv-voice\Scripts\python.exe server\configure_voice.py
```

1. 主菜单选择“新闻 API 设置（易源 ShowAPI）”，将开关设为 `true`。默认频道为“国内最新”，可改成官方返回的准确频道名称，也可设为 `/all` 跨频道；每轮最多 1–5 条，超时 1–20 秒。
2. 选择“填写当前组合的密钥”，选择“易源 ShowAPI 新闻 AppKey / `SHOWAPI_APP_KEY`”并安全输入。不要把 Key 发在聊天中。Key 从 [ShowAPI 控制台](https://www.showapi.com/console#/myApp) 获取，账号需要有新闻 API 109 权限。
3. 选择“检查已保存配置”，然后重启 Python 语音服务。只改服务端配置，不需要重建或烧录 ESP32。

ShowAPI 使用 `appKey` 查询参数认证和 HTTPS POST 表单请求；Key 只由服务端读取，不发送到设备。文档路径为 `https://route.showapi.com/109-35`（按频道/标题检索）、`/109-36`（按发布时间检索）、`/109-37`（热搜）。已限制结果数、响应字节、等待时间和输出字段；外部内容只给当前 LLM 回合，不写进长期对话历史。标题/摘要只是来源资料，助手不能由标题推断正文，也不会把抓取时间当成新闻发生时间。热搜接口没有发布时间，且官方文档不同页面对更新频率描述不一致，因此不会称它为“今天刚发生”。

可试着问：“今天有什么新闻”“播报三条科技新闻”“英伟达最新新闻”“看看热搜”或“有哪些新闻频道”。指定人物/事件并询问真假、原因或进展时继续使用现有 AnySearch 检索，不会拿频道标题代替事件核实。未注明的相对时间不会被猜测；只支持今天、昨天、前天和不超过 15 天的明确日期范围。

当前本机新闻搜索已有用户实测可用的结果；新账号部署仍需单独确认授权并填写有效 Key。本地 `--check-config` 只检查配置和凭证格式，不会调用 ShowAPI。完整接入契约见官方 [109 OpenAPI](https://www.showapi.com/openapi/market/109.json)、[频道查询](https://www.showapi.com/apis/news-channels-channel-109-34)、[新闻查询](https://www.showapi.com/apis/news-channels-query-109-35)、[按发布时间查询](https://www.showapi.com/apis/news-channels-search-publish-time-109-36) 和[热搜新闻](https://www.showapi.com/apis/news-channels-hot-search-109-37)。

`active_profile` 只决定没有显式指定组合时的默认值。**设备显式请求 `realtime_default`，因此只改 `active_profile`、启动的 `--profile` 或 `--tts` 可能不会改变设备模型**；应编辑设备请求的 profile，或在固件配置中更改 `VOICE_SERVER_PROFILE` 并烧录。现有命令行覆盖仍用于未指定 profile 的客户端和 PC 调试。

**Wi-Fi、服务电脑 IP、音量在哪改？** 在 ESP-IDF PowerShell 中运行：

```powershell
idf.py -B build\barge-in-test menuconfig
```

进入 `Voice Assistant → Basic setup`：设置 Wi-Fi 名称/密码、服务器地址、profile、麦克风增益、播放音量和按键。服务电脑 IP 改变时，设备 URL 必须同步修改；设备的 `localhost` 指设备自身，不能填写 `127.0.0.1` 连接电脑。曼波 provider 的 `127.0.0.1:9880` 则指运行 Python 服务的电脑，可以保留。保存固件配置后按[操作指南](../docs/operations.md#构建烧录与启动)构建、烧录。回声消除、VAD、插话阈值等位于 `Advanced audio and diagnostics`，先保留现有值。

硬件外设接线统一在 `main/app/board_pins.hpp`，按键 GPIO 使用 menuconfig。改接线前核对实物，相关驱动与按键冲突检查会共用同一份引脚定义。只改服务端模型不需要 menuconfig 或烧录。

**遇到报错**：

| 提示/现象 | 操作 |
| --- | --- |
| `placeholder` | 所选条目仍有 `<...>` 模板文字，先在菜单 4 填真实值 |
| `Missing selected-provider settings` | 按提示的变量名填写真实凭证；`paste-your-...` 示例文字不能当密钥 |
| `requires ... streaming ASR` | 手动切了整句识别但保留强制流式标志，改用向导切换 ASR 或同步设为 `false` |
| `Invalid JSON ... line ... column ...` | 按行列检查引号/逗号/注释，用 UTF-8 保存；凭证错误不会输出密钥所在行 |
| `tts_reason=connection_refused` 且使用曼波 | 检查 9880 引擎窗口是否持续运行，见[本地曼波](#本地曼波-tts) |
| 保存后仍用旧模型 | 重启 8000；核对设备 profile 与旧 `model_ref` 覆盖 |

### 原有配置与命令行

`config/voice_config.json` 是当前本地业务配置，已配置豆包 ASR、DeepSeek 与曼波模型参考音频；本地 `profiles.realtime_default.tts=mambo_local`。`config/voice_config.example.json` 是模板，其中 `realtime_default` 仍为火山 TTS；只有本地配置不存在时才读取模板。默认 `active_profile=legacy_http` 保留旧启动行为，使用下方命令选择实时组合。已有本地配置应直接编辑，避免用模板覆盖。

`config/server_secrets.json` 延续原有本机凭证；新凭证名称见 `server_secrets.example.json`，也可用环境变量设置。环境变量优先，未选择的服务无需凭证。业务文件仅保存 `credential_ref` 等引用名。

服务模型的选择写在业务配置中：ASR 的 `model=bigmodel` 是接口要求，`resource_id=volc.seedasr.sauc.duration` 选择流式识别 2.0 小时版；若账号开通的是并发版则使用 `volc.seedasr.sauc.concurrent`。可选火山 TTS 的 `resource_id=seed-tts-2.0` 选择语音合成 2.0，`voice=zh_female_vv_uranus_bigtts` 选择 Vivi 2.0。标准音色无需另写 TTS `model`，云端使用其默认版本 `seed-tts-2.0-standard`；`speaker` 是必填参数，因此 Vivi 是本项目保留的火山音色，并非省略音色后的接口默认。依据见 [ASR 文档](https://docs.volcengine.com/docs/doubao-voice/bidirectional-streaming-automatic-speech-recognition-websocket?lang=zh)和 [TTS 文档](https://docs.volcengine.com/docs/doubao-voice/bidirectional-streaming-text-to-speech-websocket?lang=zh)。

先检查再启动：

```powershell
# 旧 HTTP 契约：/ping；/audio 接收 PCM16 mono16k，返回识别文本和回答两行文字
.\.venv-voice\Scripts\python.exe server/voice_server.py --mode http --profile legacy_http --check-config
.\.venv-voice\Scripts\python.exe server/voice_server.py --mode http --profile legacy_http

# 当前本地组合：豆包 ASR → DeepSeek → 曼波 TTS；先启动下方 9880 引擎
.\.venv-voice\Scripts\python.exe server/voice_server.py --mode realtime --profile realtime_default --check-config
.\.venv-voice\Scripts\python.exe server/voice_server.py --mode realtime --profile realtime_default

# 独立覆盖某层，其余继承 profile；所选模型配置必须有效
.\.venv-voice\Scripts\python.exe server/voice_server.py --mode realtime --profile realtime_default --llm qwen_default --tts cosyvoice_default
```

也可使用启动脚本，它依次寻找 `.venv-voice`、`.venv-asr-fixed`、PATH 中的 Python。本机系统执行策略会阻止直接运行脚本，可只对本次 PowerShell 进程指定执行方式：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\server\start_voice_server.ps1 -Mode realtime -Profile realtime_default -CheckConfig
```

用 `--config` / `-Config` 和 `--secrets` / `-Secrets` 指定其他文件。此前火山 ASR/TTS 凭证、实时配置和原生 Opus 加载检查通过，基础短句及完整 PC 实时链路已有真实云调用记录，结果保存在忽略的 `data/` 中；当前曼波组合已由用户完成设备实测，运行时仍须同时保持引擎与语音服务在线。

LLM SSE 默认使用 1024 token 预算，旧非流式默认仍为 128；可在业务配置对应 LLM 条目中用 `max_tokens` 覆盖。length 等异常结束会明确失败，不把已截断的回答当成完整成功，也不自动重试已经开始播报的回复。

### 当前日期与 AnySearch 联网查询

实时语音服务每轮向 LLM 提供服务端当前北京时间（Asia/Shanghai／UTC+8）。独立的“今天是多少号／星期几／现在几点”由 Python 读取电脑时钟直接回答，仍走原来的完整正文、TTS 和字幕流程。电脑的系统日期必须正确；本次没有改 ESP32 固件或曼波引擎。

本地配置及模板的 `llm.deepseek_default.enable_web_search=true` 开启 AnySearch REST 预搜索；省略或设为 `false` 时不调用搜索。明确的“联网／搜索／查一下”、新闻、突发、最新、时效信息，以及“事件变化 + 求证／追问”（如“某 CEO 卸任了怎么回事”）会触发查询；普通概念问答和闲聊沿用原路径。规则按当前问题判断，并非通用工具代理；追问“还有呢”或省略事件名时，建议重复主题以便查准。此功能只接入 `/ws/voice`，旧 `/audio` 契约继续使用原链路。

服务端直接调用 [AnySearch Search API](https://anysearch.com/docs/api-endpoints/v1-search)，无需 MCP 或额外安装客户端。每次只发送当前问题和本轮北京时间日期，最多等待 20 秒，接收最多 512 KiB，取最多 5 条有界标题、摘要及来源 URL；网页内容按外部资料处理，不作为系统指令。搜索资料只用于本轮，不写入会话历史。LLM 被要求只总结资料支持的事实、简要提及来源，不把无发布时间的网页说成刚刚发生的新闻；搜索本身不保证所有结果及时或准确。

API Key 可选。本地服务默认可匿名调用，限额较低；要使用自己的账号，在忽略的 `server/config/server_secrets.json` 中增加 `"ANYSEARCH_API_KEY": "你的密钥"`，或给启动窗口设置同名环境变量后再启动。环境变量优先（包括显式空值），不自动读取 Codex 技能目录的 `.env`。密钥文件已有 ASR／LLM 凭证，只添加这一项，不要用示例覆盖整份文件。

每轮有一条不含识别正文的 `request_route route=date/search/chat search_enabled=...`，可分清日期直答、命中搜索和普通聊天；搜索开始记录 `web_search status=pending`。搜索限流、超时、空结果或格式错误时，直接显示并播报暂时无法核实，继续保留手动阅读和下一轮对话；不让 LLM 在这类故障后猜新闻。HTTP 402 等错误正文可能含自动生成凭证，服务不读取、不记录、不保存它。服务日志 `web_search status=ok results=...` 表示成功取得资料，`status=unavailable reason=... http_status=...` 或 `status=disabled` 表示降级；日志不含问题、网页正文、URL 或密钥。

用户已确认命中关键词时 AnySearch 和新闻搜索能够正常触发并使用。以后修改配置或更换账号时，可在重启窗口 B 的 8000 服务后，分别问“今天是多少号”“今天有什么新闻”“给我讲个笑话”，并验证搜索等待时键1取消、下一轮正常。观察新闻来源和日期是否明确、普通聊天是否没有搜索日志。窗口 A 的曼波保持运行，无需构建或烧录；标准启动命令见上方。配置检查不会调用搜索，匿名限额或密钥可用性须以真实查询为准。

实时回答现在先收齐 LLM 输出，再通过有界 `llm.delta` 和 `llm.done` 让设备一次提交正文，之后才启动 TTS 和音频。正文不会随字幕批次缩短或追加；首音等待包含整篇回答的生成时间。从旧版本升级时需配套更新固件并重启服务，新模式和旧版兼容边界见[设备协议](../docs/voice_protocol.md#下行与取消)。设备保留最多 8191 UTF-8 字节，超长回答明确显示截断提示，不能无限保留全文。

火山 `seed-tts-2.0` 默认请求 `enable_subtitle=true`，从返回事件提取 24 kHz 有效 PCM 的逐词时间戳。PCM 仍在原有四秒有界队列中，向传输层按 20 ms 切片交付，已到达的字幕优先于剩余 PCM。服务端将词位置保守映射到已提交的回答正文，当前 `response_word_timestamps` 模式只发时间与正文范围，不用 TTS 字幕覆盖文字。字段与容量见[字幕协议](../docs/voice_protocol.md#播报字幕扩展)。规范化差异不能可靠对齐或元数据非法时，设备退出跟随、全文保留、音频继续；没有字幕时在流结束前通知。其他 TTS 或显式设置 `enable_subtitle=false` 时仍可阅读完整回答并手动翻页。用户已确认当前三行字幕功能正常；全文预载不保证词时间戳提前到齐，更换 TTS、参数或设备环境后仍应按需观察同步效果。

TTS2.0 的句子开始／结束事件 350／351 与独立字幕事件分开处理，边界消息不参与字幕解码；其他事件的空 `words` 继续等待真正的逐词时间戳。此前已修复 `tts_subtitle_invalid event=350 reason=invalid_schema` 导致整轮字幕失效的问题，该项本身只涉及服务端；当前完整正文与新 UI 则需要更新固件。服务终端 `subtitle_state status=following`（已下发首批有效字幕）或 `status=unavailable reason=... detail=...` 区分上游非法数据、组装错误和流结束无字幕；不记录正文。实机复验步骤见[操作指南](../docs/operations.md#三行字幕随播报自动翻页)。

“一段一段出现／停住后追赶”的定位另使用 `tts_subtitle_timing`、`tts_audio_backpressure` 和 `subtitle_timing`，逐批关联上游本地接收、PCM 背压、transport 排队和下行发送。它们复用有界后台日志，不记录正文，内部接收时间和来源标识不进入设备协议。已消费／已发送 PCM 内容均不等于扬声器实际播放进度；字段和判读见[字幕时序诊断](../docs/operations.md#字幕成段出现与时序诊断)。当前已拆开正文供给和跟随时间戳，未增加音频／字幕协同预缓冲。

`--list-options` 不读取密钥文件、不调用云服务；`--check-config` 只检查所选配置、必需凭证是否存在，以及配置为 Opus 时原生库是否能加载，不监听端口、不调用云服务。它不能证明账号权限或音色有效。

## PC 联调与离线测试

PC 模拟 ESP32：准备一个 16 kHz、单声道、PCM16 WAV 文件，服务运行后执行：

```powershell
.\.venv-voice\Scripts\python.exe tools/voice_client.py server/data/question.wav --output server/data/reply.wav
# 显式 PCM 回退；服务配置需允许回退
.\.venv-voice\Scripts\python.exe tools/voice_client.py server/data/question.wav --codec pcm_s16le
# 在开始回复后发起手动取消
.\.venv-voice\Scripts\python.exe tools/voice_client.py server/data/question.wav --cancel-after-ms 300
# 同连接两轮，第一轮收到首音频后取消，第二轮继续
.\.venv-voice\Scripts\python.exe tools/voice_client.py server/data/question.wav server/data/next_question.wav --cancel-trigger first-audio --cancel-after-ms 0 --output server/data/multiturn.wav --report server/data/multiturn.json
```

该工具等待对应 `speech.started` 后以 20 ms 节奏上传，发送明确的 `speech.start` / `speech.end`。多个输入在同一连接内逐轮发送；多轮输出为 `*_turn01.wav` 等 24 kHz WAV。`--cancel-turn` 指定取消轮次，默认第一轮；计时起点默认 `response-start`，也可选 `first-audio`。工具不会播放音频或发送 `playback.done`。向真实服务发送录音会调用所选云服务。

默认在输出 WAV 旁保存同名 JSON（可用 `--report` 指定），包含模型标识、转写、回答、逐阶段时刻与音频 payload/协议头字节数，不含密钥/端点。字节数不包含 WS/TCP/TLS 开销；时间戳是 PC 收发时间，不是设备播放时间。完整成功要求非空 ASR、非空回答、正常结束的非空音频；错误或未发生预期取消会以非零状态退出。

离线测试：

```powershell
.\.venv-voice\Scripts\python.exe -m unittest discover -s server/tests -t server -v
```

原 `audio_test_server.py` 入口保留，可用标准库 Python 运行；其 Qwen/DeepSeek 实现复用适配器。实时模式同时保留旧 `/ping`、`/audio`，后者仍使用原 Qwen → DeepSeek 两行文本契约。实时 WS 模型选择不改变该旧接口。

## 诊断、超时与队列

实时诊断输出使用 `voice_failure first=1` 保留连接首因，并在 `voice_asr_summary` 汇总有界队列等待、解码、ASR feed、进行中 socket 发送、Python 事件循环延迟和包到达突发。所有值为数字及固定分类，不打印音频、转写、密钥或远端错误正文。`asr_inflight=1` 表示发送仍未完成，不能只看已完成发送的 `asr_send_ms`。新增 `queue_oldest_audio_ms` 表示仍在队列中的最老音频年龄，`worker_phase` 表示当前消费阶段；`turn_receive_*` 只统计当前发言，原接收峰值及阶段最大值按连接累计。字段解释和历史复验记录见 [P3 问题记录](../docs/archive/p3/p3_recording_disconnect_issues.md)。

每轮另有 `voice_asr_result`，记录 `outcome`、最终字符数、豆包响应/非空响应数、成功写给设备的识别事件数，以及实际送入ASR的PCM总量、RMS、去直流RMS、峰值和削顶数；不保留录音或输出文字内容。`provider_final=1 final_chars=0 outcome=empty`与缺失最终、超时失败分别记录。成功写入设备不代表已渲染；具体字段及历史复验见[空识别与显示排查](../docs/archive/p4/p4_empty_asr_result.md)。

`config/voice_config.json` 的 `transport.idle_timeout_seconds` 默认为 **0**，设备可以长期待机，不必持续上传静音。也可设为 1–86400 秒；有限待机到期会发送正常的 `session.closed / idle_timeout` 并以 1000 关闭，设备会自动按当前暂停／监听意图建立新会话。Ping/Pong 不刷新业务空闲时间；录音、生成和播放保护期间不按待机关闭。该设置与各云服务的操作超时分开。设备端的重试上限和人工重试按[操作指南](../docs/operations.md#双按钮与独立操作)执行。

ASR 故障日志还包含固定 `asr_reason`、数字 `asr_upstream_code` 和 `asr_upstream_close_code`，用于区分输入停滞、握手响应超时、云端拒绝和云连接关闭；不打印远端正文。豆包 `timeout_s` 默认 30 秒：握手与每次发送保留操作期限；录音期间按最近一次成功发送非空音频的时间判断输入停滞（`input_timeout`），允许没有中间识别结果；发送结束包后再给最终结果完整的 30 秒预算（`final_timeout`）。只收到真正的空最终结果时，才返回 `empty_utterance`，不因超时伪造空识别。上述期限与适配器默认 60 秒的 `max_utterance_s`、固件 30 秒自动提交上限分开。具体排查见 [录音断开问题第 12 节](../docs/archive/p3/p3_recording_disconnect_issues.md#12-2026-11-10确认响应等待超时并修复)。

上行队列仍为 25 项；满队列先等待最多 100 ms 并给消费任务运行机会，排队年龄和相对媒体落后量的检查阈值为 750 ms。后者从本轮首个音频包开始，将样本时间线与服务端耗时比较，避免只看应用队列而漏掉网络缓冲内逐渐增长的积压；不测量首次音频到达前的固定延迟。日志补充 `turn_media_lag_ms/max_ms` 与 `worker_media_lag_ms`。超限返回 `uplink_backlog`，不会丢 Opus 包后继续或自动重发。100 ms 应用等待不保证端到端网络延迟低于设备发送预算，进行中的云调用仍受各自超时控制。更新服务代码或配置后需重启原服务端进程生效。


## 本地曼波 TTS

本节面向第一次从 GitHub 下载本项目、希望使用曼波音色的用户。这里的“本地 TTS”指在**电脑**运行语音合成模型，通过 HTTP 返回音频；ESP32 负责录音、联网、播放和显示，不运行曼波模型。

本仓库只包含接口适配代码，不提供曼波/GPT-SoVITS 引擎本体、音色权重、预训练资源或参考音频。它们需要从各自上游另行获取，按原许可使用；本项目的代码许可不代替模型、音色和参考录音的使用授权。曼波合成本身可在本地完成，但默认豆包 ASR 和 DeepSeek LLM 仍需要联网及你自己的 API 凭证，不能因此把整个助手当成离线助手。

### 1. 安装引擎并准备文件

本项目已核对的接入对象是 **MamboTTS v1.2.1 的旧版 GPT-SoVITS `api.py` 接口**。下载入口来自 MamboTTS 自带 README：[MamboTTS 发布页](https://github.com/Tsukimisaka/MamboTTS/releases)，对应发布包为 `MamboTTS-v1.2.1-full.zip`；引擎上游为 [RVC-Boss/GPT-SoVITS](https://github.com/RVC-Boss/GPT-SoVITS)。其他版本能否使用，以是否满足下方接口契约为准。

上游一键包面向 **Windows 10/11 64 位与 NVIDIA GPU**。按上游说明，首次启动需要下载约 8.2 GB 引擎，建议预留 30 GB 以上磁盘空间，并解压到全英文路径；具体下载体积和硬件要求以该版本上游说明为准。CPU、AMD 显卡、macOS/Linux 或自行搭建的 GPT-SoVITS 不属于本项目已验证的一键安装流程，不能保证性能或直接兼容。

下载解压后，按上游说明运行 `MamboTTS.bat` 完成依赖及引擎安装。它可能同时启动图形界面和占用 9880 的引擎。若接下来使用本节的手动启动方式，先正常关闭该图形界面及其引擎，避免同一端口启动两份进程；不要在助手运行期间关闭正在提供合成的引擎。

下面使用两个**示例安装位置**，都可换成自己的路径，不要求与作者的 `C:\esp-project` 相同：

```text
D:\projects\voice_assistant\           本仓库，包含 server\voice_server.py
D:\apps\MamboTTS-v1.2.1-full\          单独安装的曼波
├── GPT-SoVITS\
│   ├── api.py                          本项目使用的旧 API 入口
│   └── runtime\python.exe             曼波附带的 Python
└── models\
    ├── manbo_e8_s168.pth               SoVITS 音色权重
    ├── manbo-e10.ckpt                  GPT 音色权重
    └── refer.wav                      配套参考音频
```

先确认这些文件以及引擎所需的预训练资源安装完整；仅下载本仓库或只放入两份权重还不够。若上游包的模型文件名不同，启动命令的 `-s` 和 `-g` 必须对应实际文件，不能随意把不同模型版本配在一起。

### 2. 配置本机模型组合

先按[环境与依赖](#环境与依赖)为语音助手创建 `.venv-voice` 并安装 `server/requirements.txt`；它与曼波的 `runtime/python.exe` 是两个环境，不要混用。在 PowerShell 中切到自己的项目目录：

```powershell
$assistantRoot = 'D:\projects\voice_assistant'
Set-Location -LiteralPath $assistantRoot
.\.venv-voice\Scripts\python.exe server\configure_voice.py
```

如果尚无本地业务配置，向导会从模板读取，首次保存才创建 `server/config/voice_config.json`；已有配置不会被模板覆盖。按以下顺序配置：

1. 菜单 4 → TTS → `mambo_local`，把 `refer_wav_path` 改成你实际的参考音频**绝对路径**，例如 `D:/apps/MamboTTS-v1.2.1-full/models/refer.wav`。它是 9880 引擎所在电脑的路径，不是 ESP32 的路径。
2. 核对 `prompt_text` 与这段参考音频里说的内容一致；使用其他音色或参考录音时，同时修改 `prompt_language`、`text_language` 等参数。默认配套参考音频的转写为“大家好，欢迎来到我的频道，今天给大家分享一个有趣的内容”，仅在确实使用该录音时保留。
3. 菜单 3 → 选择 `mambo_local` 并保存。向导默认修改的是设备请求的 `profiles.realtime_default.tts`。
4. 菜单 5 → 填写自己 ASR 和 LLM 所需凭证。曼波当前本机 HTTP API 不需要云 TTS Key，但默认豆包 ASR 和 DeepSeek 仍需对应 Key。
5. 菜单 6 → 检查已保存配置；完成后退出向导。

对应配置的关键字段如下，表中“字段路径”仅用于定位，不能把这张表直接粘进 JSON：

| 字段路径 | 应填内容 |
| --- | --- |
| `profiles.realtime_default.tts` | `mambo_local` |
| `tts.mambo_local.adapter` | `gpt_sovits` |
| `tts.mambo_local.endpoint` | 同机时为 `http://127.0.0.1:9880/` |
| `tts.mambo_local.refer_wav_path` | 9880 引擎电脑上的参考 WAV 绝对路径 |
| `tts.mambo_local.prompt_text` | 与参考录音一致的转写文字 |
| `tts.mambo_local.prompt_language` | 参考录音语言，中文示例为 `zh` |
| `tts.mambo_local.text_language` | 待播报文字语言，中文示例为 `zh` |
| `tts.mambo_local.speed` | 默认 `1.0`；先用正常语速验证，再按引擎能力调整 |

**模板与作者本机不同：** 公开模板中的 `profiles.realtime_default.tts` 是 `volc_default`，独立 `profiles.mambo_local.tts` 才是 `mambo_local`。作者本机已手动把 `realtime_default` 切到曼波，这个本地配置不会随仓库发布。设备会在 `session.start` 显式请求 `realtime_default`，仅给服务启动命令加 `--profile mambo_local` 或 `--tts mambo_local` 不会覆盖设备选择；应按上述步骤修改本地 `profiles.realtime_default.tts`。只改服务端模型组合不需要重建或烧录固件。

### 3. 窗口 A 启动 9880 曼波引擎

新开 PowerShell，替换第一行安装位置，然后复制执行。下面假定使用上图中的模型和配套参考录音；更换录音时，`-dr/-dt/-dl` 也要同步修改：

```powershell
$mamboRoot = 'D:\apps\MamboTTS-v1.2.1-full'
Set-Location -LiteralPath (Join-Path $mamboRoot 'GPT-SoVITS')
.\runtime\python.exe .\api.py `
  -a 127.0.0.1 -p 9880 `
  -s ..\models\manbo_e8_s168.pth `
  -g ..\models\manbo-e10.ckpt `
  -dr ..\models\refer.wav `
  -dt "大家好，欢迎来到我的频道，今天给大家分享一个有趣的内容" `
  -dl zh `
  -fp -sm close -mt wav -st int16 `
  -cp "，。？！；：、…,.;?!"
```

看到 `Uvicorn running on http://127.0.0.1:9880` 表示引擎已经监听，但还需要实际合成验证模型可用。**窗口 A 必须保持运行**，不要按 Ctrl+C；后面另开窗口 B。出现 `Shutting down` / `Finished server process` 表示引擎已退出。

这组参数保持本项目已有的全精度、非流式 WAV/PCM16 输出，并按标点切分。不要直接替换成 `api_v2.py /tts`；也不要改为 `-sm normal`，当前这份引擎在该模式会输出 OGG，适配器不能按 WAV 解码。

### 4. 窗口 B 检查并启动 8000 语音服务

新开第二个 PowerShell，独立设置项目路径；窗口 A 中的变量不会自动传入窗口 B：

```powershell
$assistantRoot = 'D:\projects\voice_assistant'
Set-Location -LiteralPath $assistantRoot
.\.venv-voice\Scripts\python.exe server\voice_server.py `
  --mode realtime --profile realtime_default --check-config
.\.venv-voice\Scripts\python.exe server\voice_server.py `
  --mode realtime --profile realtime_default
```

检查摘要应包含 `"profile": "realtime_default"` 与 `"tts": { "id": "mambo_local", ... }`。`--check-config` 不监听服务、不连接 9880，也不执行合成；它只验证所选配置、必需凭证及 Opus 本机库。随后第二条命令才启动语音服务。

两个窗口都需持续运行。修改模型配置后，在**窗口 B** 按 Ctrl+C 停止旧服务，再执行启动命令；窗口 A 保持运行。若 8000 已运行且配置正确，只补开 9880 引擎即可重新对话。

### 5. 验证合成与设备播放

首次排查时，先让窗口 A 的引擎保持运行，在另一个临时 PowerShell 窗口单独合成一条短句，排除 ASR、LLM 和设备网络因素。路径、转写和语言要与自己实际资源相同：

```powershell
$assistantRoot = 'D:\projects\voice_assistant'
$mamboRoot = 'D:\apps\MamboTTS-v1.2.1-full'
Set-Location -LiteralPath $assistantRoot
New-Item -ItemType Directory -Force -Path '.\server\data' | Out-Null
$payload = @{
  text = '你好，这是曼波语音合成测试。'
  text_language = 'zh'
  refer_wav_path = (Join-Path $mamboRoot 'models\refer.wav')
  prompt_text = '大家好，欢迎来到我的频道，今天给大家分享一个有趣的内容'
  prompt_language = 'zh'
  speed = 1.0
} | ConvertTo-Json -Compress
$utf8Body = [System.Text.Encoding]::UTF8.GetBytes($payload)
Invoke-WebRequest -UseBasicParsing `
  -Uri 'http://127.0.0.1:9880/' -Method Post `
  -ContentType 'application/json; charset=utf-8' -Body $utf8Body `
  -OutFile '.\server\data\mambo_test.wav' -TimeoutSec 120
```

用播放器打开生成的 WAV，确认不是静音、音色正常且语句完整。这一步会在你的电脑实际运行模型，但不经过 ESP32；`server/data/` 的测试音频已被忽略，不应提交到 GitHub。端口可连接、浏览器能打开页面或文件存在，都不能单独证明语音合成正常。

再让 ESP32 连接电脑 **8000** 服务，说一句短问题，确认识别文字、回答、播报及取消后下一轮正常。设备端服务器 URL 应使用这台电脑可达的局域网 IP，不能写 `127.0.0.1`；它不直接连接 9880。两项本地服务在同一台电脑时，曼波配置中的 `127.0.0.1:9880` 可以保留。

若引擎和语音服务分布在两台电脑，必须同时处理引擎监听地址、局域网访问和 `endpoint`；参考 WAV 路径按引擎电脑解释。当前旧 API 没有鉴权层，示例仅绑定本机回环，不要直接映射到公网；远程部署需要另行管理访问控制，不属于上面的一键本机步骤。

### 常见问题与替代方案

| 现象 | 检查方法 |
| --- | --- |
| 下载本仓库后找不到 `runtime/python.exe`、权重或 `refer.wav` | 它们来自单独的曼波安装包，完成步骤 1，不要到语音助手虚拟环境里找引擎文件 |
| 9880 提示地址已被占用 | MamboTTS 图形界面可能已启动引擎；确认现有进程归属，正常停止重复实例后只保留一个引擎 |
| `tts_reason=connection_refused` | 检查窗口 A 是否仍运行、端口与 `endpoint` 是否一致；只有 8000 在运行不能合成 |
| 找不到参考音频或音色不对 | 修改 `refer_wav_path` 为引擎电脑上的实际路径；核对两份模型和参考录音/转写是否配套 |
| `invalid_wav` / `unsupported_wav` | 核对旧版 `api.py` 的 `POST /`，并保留 `-sm close -mt wav -st int16` |
| `http_status` | 查看 `tts_http_status` 及窗口 A 的报错；不要把远端错误正文当作音频保存 |
| `timeout` / 显存不足 / 合成很慢 | 检查引擎终端、显卡驱动及整合包兼容性；先测短句，降低并发。单改 TTS 超时不能突破整轮 120 秒预算 |
| `--check-config` 通过但不能播报 | 离线检查不调用引擎，按步骤 5 做真实合成，再排查设备链路 |
| 仍在使用火山音色 | 核对本地 `profiles.realtime_default.tts=mambo_local`，并重启 8000 服务，不要只改启动 `--profile` |
| 没有适用显卡或不想安装曼波 | 菜单 3 切换到 `volc_default` 等已支持的云 TTS，填自己的对应凭证并重启 8000；云服务权限、音色和费用按服务商规定 |

GPT-SoVITS 返回的音频会被转换为 24 kHz 单声道 PCM16，再复用现有 Opus/PCM 下行。曼波适配器提供的是**文本段级播放时间范围**，不是模型逐字时间戳；设备依据真实 DMA 时钟估算段内阅读进度，长停顿可能带来翻页误差，对齐失败时仍可手动翻页。日志 `subtitle_state status=following` 只表示字幕信息已经映射并发送，实际同步需观察屏幕与播报。

切换本地 TTS、修改参考路径或整理本说明均不需要重建/烧录 ESP32；只有同时修改设备端服务器地址等固件配置时，才按[操作指南](../docs/operations.md#构建烧录与启动)执行对应操作。

### 长回答首音等待

当前仍先生成完整 LLM 回答并提交显示，随后把播报文本按标点分段合成：本地 `tts.mambo_local.first_segment_chars=40`、`segment_chars=80`，分别限制首段与后续段的字符数；两项均支持 8–200 的整数，缺省也为 40／80。首段 WAV 完成后立刻下发，同时只预取下一段；每次最多一个 HTTP 合成请求，缓存限制在当前段和下一段，所有段共用同一个设备音频流。现在首音无需等待整篇 TTS 完成，仍包含整篇 LLM 生成和首段合成时间。

这是旧 API 上的分段请求，不是把它切为 OGG 原生流式。每段仍返回完整 WAV，参考音频会被引擎重复处理；切得太短会增加请求开销和停顿。保留引擎原有段尾静音，不额外加静音；后段推理若慢于当前段播放，仍可能出现句间等待。用户已确认当前中文连续播报和音色连贯性正常；以后调参可从 40／80 开始比较首音与连贯性，不承诺固定秒数或总合成吞吐量提升。

`max_audio_bytes` 对整轮多个响应累计，`timeout_s` 使用首个合成请求起的总预算，不因分段重置；外层流水线仍有包含 LLM、合成和下发的 120 秒总时限。取消或关闭会取消并回收客户端当前请求／预取任务，不保证远端已经开始的 GPU 推理立即停止。后段失败不会自动重播已经听过的内容。

以后修改这些服务端分段参数，只需在窗口 B 重启 8000 语音服务，窗口 A 的 9880 曼波继续运行，固件无需重建／烧录。复测时可用同一条较长问题对比回答文字显示到首音的等待、句间空白，并验证键1取消后下一轮正常；当前版本的连续播报、音色连贯性和插话中断已获用户确认。

服务终端的 `tts_segment` 提供 `index`、`text_chars`、`request_ms`、`audio_ms` 及累计响应／PCM 字节数。`index=1` 的 `request_ms` 是第一段 HTTP 合成、下载和转换耗时，不包含之前的 LLM 或设备缓冲。后续段的 `request_ms` 持续大于上一段 `audio_ms` 时，预取很可能赶不上播放；这仍不是扬声器实际延迟测量。日志只包含固定分类和数字，不含回答或参考文本。

该分段优化交付时的离线服务端完整回归为 303 项通过，覆盖真实配置选择、真实 provider 工厂、32→24 kHz 转换、故障诊断、分段首音提前输出、单段预取、取消回收及同流连续下发；本地实时配置和 Opus 加载检查通过。`--check-config` 不检查 9880 服务在线，也不执行真实合成。用户于 2026-11-17 补充确认曼波设备测试正常，连续播报、音色连贯性、插话中断和多轮功能正常；首音约 1 秒内／差网络 2–3 秒是用户体验观察，尚无统一计时口径和分位统计。历史批次未由代理执行的构建、烧录和串口记录保持原边界。

要恢复火山，将本地 `profiles.realtime_default.tts` 改回 `volc_default`，停止并重启语音服务；无需覆盖整份本地配置或重新烧录。
