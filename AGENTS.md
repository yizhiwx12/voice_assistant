# Voice Assistant Project Requirements

These rules apply to the whole `voice_assistant` project.

## Directory Responsibilities

Keep files grouped by ownership and keep the directory tree easy to scan:

- `main/main.cpp` is only the ESP-IDF entry point.
- `main/app/` owns application initialization, orchestration, and shared assistant state.
- `main/audio/` owns microphone and speaker drivers and the audio capture/processing path.
- `main/display/` owns the ST7789 driver and LVGL user interface.
- `main/network/` owns Wi-Fi and future network/protocol modules.
- `main/fonts/` owns generated and licensed font resources.
- `server/` owns the host-side voice service. Put local configuration in `server/config/`, runtime/debug files in `server/data/`, and Python tests in `server/tests/`.
- `tools/` owns development-only generators and utilities.

Do not put credentials, runtime recordings, build output, or generated binaries in source directories. Keep local secrets and runtime data ignored by Git; transient Python caches may be created normally but must remain ignored and untracked.

## Documentation Maintenance

- Start from `docs/README.md`. Keep current status in `docs/realtime_voice_progress.md`, current commands and diagnostics in `docs/operations.md`, and architecture/milestones in `docs/realtime_voice_plan.md`.
- Keep protocol details in `docs/voice_protocol.md` and service setup/configuration in `server/README.md`; link to these instead of duplicating them.
- Update these existing documents for routine fixes. Do not create another numbered or dated repair document for every change. Put substantial standalone experiment or milestone evidence in the appropriate `docs/archive/` stage directory and update `docs/archive/README.md`.
- Preserve archived evidence and its original verification boundaries. Historical build directories and commands are not current instructions. Update links and anchors whenever documentation moves.

## Change Requirements

### User-controlled firmware operations

For this project, the user performs all firmware builds and flashing. The agent may inspect and edit source code and documentation, but must not run `idf.py build`, `idf.py flash`, `esptool write-flash`, or any other firmware build/flash command. The agent must also avoid opening or occupying the device serial port (including `monitor`).

For every firmware change that needs a new image, provide both the exact build command and the exact flash/monitor command for the selected build directory, `SDKCONFIG` file (when non-default), serial port, and baud rate. Do not give a generic command that could build the wrong firmware variant. When device testing needs the host voice service, also provide its exact start command. The standard PowerShell real-time service command is:

```powershell
cd C:\esp-project\voice_assistant
.\.venv-voice\Scripts\python.exe server\voice_server.py --mode realtime --profile realtime_default
```

The user runs these commands. Clearly distinguish code changes from user-run build, flash, service, and hardware results. For documentation-only changes, state that a firmware build is unnecessary rather than implying one was performed.

When adding or moving a file:

1. Put it in the directory that owns its responsibility.
2. Update affected build source lists, include paths, relative includes, scripts, test imports, launch commands, and documentation in the same change.
3. Search for stale references to the old path.
4. For firmware source moves, check existing build information where applicable; if it needs regeneration, provide the user with the build command and leave compilation verification pending their result.
5. Preserve current hardware pin assignments, server endpoints, and behavior unless the request explicitly changes them.

Before reporting completion, review the changed files and provide the commands required to validate them. Firmware builds and flashing are performed by the user under the rule above. Do not claim build, flash, service-start, or hardware validation success without the corresponding user-provided result.

## 中文注释约定

- 自有固件、服务端、测试、构建与开发脚本的说明性注释和文档字符串统一使用简体中文。
- 保留标识符、协议字段、厂商名称、命令、路径，以及 `noqa`、类型检查标记等工具识别内容。
- 第三方依赖、许可证声明、自动生成资源和配置中的机器标记保持原样；不将日志或接口字符串误当作注释翻译。
