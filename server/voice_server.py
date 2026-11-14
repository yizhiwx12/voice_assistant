"""服务入口：配置校验、HTTP/实时 WS 装配和统一资源关闭。

新手先运行 configure_voice.py。这里不包含厂商协议或音频算法，便于单独测试入口。
"""

import argparse
import asyncio
import json
import logging
import sys
from http.server import ThreadingHTTPServer
from pathlib import Path

from core.config import (
    ConfigurationError,
    apply_model_overrides,
    list_options,
    load_secrets,
    load_voice_config,
    news_credential_status,
    referenced_settings,
    select_profile,
    validate_selection_credentials,
)
from core.provider_registry import create_provider
from providers.llm.chat_completions import LEGACY_SYSTEM_PROMPT
from transport.http_routes import make_handler

SERVER_DIR = Path(__file__).resolve().parent


def argument_parser():
    parser = argparse.ArgumentParser(
        description="语音助手服务：ASR 识别 → LLM 回答 → TTS 播报，各层可独立选择",
        epilog="新手配置：python server/configure_voice.py。设备显式请求 profile 时，以该 profile 的模型组合为准；更改后重启服务。",
    )
    parser.add_argument("--config", type=Path, help="Business configuration JSON (default: config/voice_config.json or example)")
    parser.add_argument("--secrets", type=Path, help="Local credentials JSON; environment variables take precedence")
    parser.add_argument("--list-options", action="store_true", help="List safe model choices and actual implementation capabilities; no API calls")
    parser.add_argument("--check-config", action="store_true", help="Validate selected configuration and required setting names without binding or API calls")
    parser.add_argument("--profile", help="Named combination; defaults to config active_profile")
    parser.add_argument("--asr", help="Override ASR choice independently")
    parser.add_argument("--llm", help="Override LLM choice independently")
    parser.add_argument("--tts", help="Override TTS choice (HTTP requires 'none')")
    parser.add_argument("--mode", choices=("http", "realtime"), default="http", help="Compatible HTTP or bidirectional realtime WebSocket")
    parser.add_argument("--host", default="0.0.0.0", help="Listening interface")
    parser.add_argument("--port", type=int, default=8000, help="Listening TCP port (default: 8000)")
    return parser


def resolve_configuration(args):
    """只读一次业务文件；校验、启动摘要及所有会话共享同一个配置快照。"""
    config = load_voice_config(args.config, environ={})
    credentials = load_secrets(args.secrets, references=referenced_settings(config))
    # 同时读取凭证文件中的旧版模型变量，不修改 os.environ，
    # 也不修改其他运行中或嵌入式服务实例的配置。
    config = apply_model_overrides(config, credentials)
    selection = select_profile(
        config, profile=args.profile, asr=args.asr, llm=args.llm,
        tts=args.tts, mode=args.mode,
    )
    validate_selection_credentials(selection, credentials)
    return config, selection, credentials


def resolve_selection(args):
    """保留供脚本调用的旧接口；实际装配统一通过 resolve_configuration。"""
    _, selection, credentials = resolve_configuration(args)
    return selection, credentials


def create_http_server(selection, credentials, *, host="0.0.0.0", port=8000, opener=None):
    if selection.mode != "http":
        raise ConfigurationError("HTTP server requires --mode http; use create_realtime_app for realtime")
    asr = create_provider("asr", selection.asr, credentials, opener=opener)
    llm = create_provider("llm", selection.llm, credentials, opener=opener)

    def reply(transcript):
        return llm.generate([
            {"role": "system", "content": LEGACY_SYSTEM_PROMPT},
            {"role": "user", "content": transcript},
        ])

    handler = make_handler(asr.recognize_wav, reply, data_directory=SERVER_DIR / "data")
    return ThreadingHTTPServer((host, port), handler)


def create_realtime_app(config, credentials, selection, *, provider_factory=None):
    """创建应用对象，不打开监听端口或调用云端 API。"""
    if selection.mode != "realtime" or selection.tts is None:
        raise ConfigurationError("Realtime app requires a complete realtime selection")
    import aiohttp
    from aiohttp import web
    from transport.voice_websocket import make_voice_route
    from transport.app_state import CONNECTIONS, PROVIDER_HTTP, VoiceConnections

    app = web.Application(client_max_size=1_000_000)
    app[CONNECTIONS] = VoiceConnections()

    async def provider_http(application):
        async with aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=60)) as session:
            application[PROVIDER_HTTP] = session
            yield
    app.cleanup_ctx.append(provider_http)

    async def close_devices(application):
        await asyncio.gather(*(ws.close(code=1001, message=b"Service shutdown")
                               for ws in tuple(application[CONNECTIONS].sockets)),
                             return_exceptions=True)
    app.on_shutdown.append(close_devices)

    async def ping(request):
        return web.Response(text="OK")

    async def legacy_audio(request):
        # 即使 WebSocket 选择了其他模型，这些路由仍使用千问 → DeepSeek。
        # 复用同一组适配器，不另写一套云端协议实现。
        from audio.formats import pcm16_to_wav
        from core.text_utils import clean_single_line, truncate_utf8
        from providers.asr.qwen_batch import QwenBatchProvider
        from providers.llm.chat_completions import ChatCompletionsProvider

        length = request.content_length
        if length is None or not 0 < length <= 1_000_000 or length % 2:
            return web.Response(status=400, text="Invalid PCM16 audio length")
        pcm = await request.read()
        if len(pcm) != length:
            return web.Response(status=400, text="Incomplete audio body")
        asr = QwenBatchProvider({
            "model": credentials.get("DASHSCOPE_ASR_MODEL", "qwen-audio-3.1-asr-flash"),
            "credential_ref": "DASHSCOPE_API_KEY", "workspace_ref": "DASHSCOPE_WORKSPACE_ID",
        }, credentials)
        try:
            text = clean_single_line(await asr.recognize_wav_async(pcm16_to_wav(pcm), app[PROVIDER_HTTP]))
        except Exception:
            return web.Response(status=500, text="Speech recognition failed")
        try:
            await asyncio.to_thread(_save_legacy_audio, pcm)
        except OSError:
            return web.Response(status=500, text="Unable to save audio")
        if text:
            llm = ChatCompletionsProvider({
                "model": credentials.get("DEEPSEEK_MODEL", "deepseek-flash"),
                "base_url": "https://api.deepseek.com", "credential_ref": "DEEPSEEK_API_KEY",
            }, credentials)
            try:
                answer = await llm.generate_async([
                    {"role": "system", "content": LEGACY_SYSTEM_PROMPT},
                    {"role": "user", "content": text},
                ], app[PROVIDER_HTTP])
                answer = clean_single_line(answer)
                if not answer:
                    raise ValueError("Empty answer")
            except Exception:
                return web.Response(status=502, text="DeepSeek request failed")
        else:
            text, answer = "未识别到语音", "请靠近麦克风后重试。"
        return web.Response(text=f"{truncate_utf8(clean_single_line(text), 96)}\n"
                                 f"{truncate_utf8(clean_single_line(answer), 120)}")

    app.router.add_get("/ping", ping)
    app.router.add_post("/audio", legacy_audio)
    app.router.add_get("/ws/voice", make_voice_route(config, credentials, selection,
                                                   provider_factory=provider_factory))
    return app


def _save_legacy_audio(pcm):
    directory = SERVER_DIR / "data"
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "received_audio.pcm").write_bytes(pcm)


def _configure_realtime_logging(*, stream=None, capacity=128, stop_timeout=0.5):
    # 只启用项目的固定字段诊断日志；HTTP 访问日志保持关闭，
    # 上游客户端也不会继承详细日志设置。
    logger = logging.getLogger("transport.voice_websocket")
    logger.setLevel(logging.INFO)
    logger.propagate = False
    from transport.diagnostic_logging import BoundedDiagnosticHandler
    for existing in logger.handlers:
        if isinstance(existing, BoundedDiagnosticHandler) and existing.stats().accepting:
            return existing
    # 本命令行入口独占专用日志器；启用非阻塞输出后，
    # 不再保留此前安装的同步终端处理器。
    for existing in tuple(logger.handlers):
        logger.removeHandler(existing)
        if isinstance(existing, BoundedDiagnosticHandler):
            existing.close()
    handler = BoundedDiagnosticHandler(stream, capacity=capacity, stop_timeout=stop_timeout)
    logger.addHandler(handler)
    return handler


def main(argv=None):
    parser = argument_parser()
    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    try:
        if args.list_options:
            print(json.dumps(list_options(load_voice_config(args.config)), ensure_ascii=False, indent=2))
            return 0
        config, selection, credentials = resolve_configuration(args)
        print(json.dumps(selection.to_dict(), ensure_ascii=False, indent=2))
        news_status = news_credential_status(selection, credentials)
        if news_status == "missing_credentials":
            print("新闻功能等待配置：请在本地 server_secrets.json 或环境变量填写 "
                  + selection.news.get("credential_ref", "SHOWAPI_APP_KEY")
                  + "；普通语音对话仍可使用。")
        elif news_status == "ready":
            print("ShowAPI 新闻配置已就绪；AppKey 权限和真实返回仍需联网验证。")
        if args.check_config:
            if args.mode == "realtime" and "opus" in (
                selection.transport["uplink"]["codec"], selection.transport["downlink"]["codec"]
            ):
                from audio.opus_codec import native_version
                print("Opus native library: " + native_version())
            print("Configuration valid; no provider API was called.")
            return 0
        if args.mode == "realtime":
            from aiohttp import web
            diagnostic_handler = _configure_realtime_logging()
            try:
                # 本次运行固定使用同一份配置快照；文件修改后须明确重启才生效，
                # 不会在对话进行中切换配置。
                app = create_realtime_app(config, credentials, selection)
                web.run_app(app, host=args.host, port=args.port, access_log=None)
            finally:
                logging.getLogger("transport.voice_websocket").removeHandler(diagnostic_handler)
                diagnostic_handler.close()
            return 0
        server = create_http_server(selection, credentials, host=args.host, port=args.port)
    except ImportError:
        print("Realtime dependencies missing; install server/requirements.txt in the service environment.", file=sys.stderr)
        return 2
    except (ValueError, RuntimeError) as exc:
        print(f"Configuration error: {exc}", file=sys.stderr)
        return 2
    except OSError:
        print("Unable to bind HTTP service; check the listening address and port.", file=sys.stderr)
        return 1

    print(f"HTTP voice service ready at http://{args.host}:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
