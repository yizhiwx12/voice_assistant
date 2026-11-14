"""每条设备连接独立的 ASR → LLM → TTS 协调器。

录音由 start/feed/end 驱动；识别结束后生成完整回答，再开始合成和下发。
厂商协议放在 providers，音频帧/播放确认放在 transport，本模块管理回合
及资源生命周期。取消时先回收旧任务，避免旧声音或字幕串入下一轮。
"""

import asyncio
import logging
from contextlib import suppress

from audio.formats import pcm16_to_wav
from core.session import ConversationSession
from core.text_segmenter import TextSegmenter
from core.realtime_info import (deterministic_date_answer, needs_realtime_search,
                                search_query, with_search_results)
from providers.search.anysearch import AnySearchError, AnySearchProvider
from core.config import news_credential_status
from core.news_routing import is_news_discussion, news_request, with_news_results
from providers.news.showapi import ShowApiNewsError, ShowApiNewsProvider


_LOG = logging.getLogger("transport.voice_websocket.search")


class VoicePipeline:
    """一条 WebSocket 对应一个实例；共享 HTTP 会话由服务入口负责关闭。"""
    def __init__(self, selection, credentials, http_session, emit, send_audio,
                 *, provider_factory=None):
        self.session = ConversationSession(selection)
        self.credentials = credentials
        self.http = http_session
        self.emit_callback = emit
        self.send_audio = send_audio
        if provider_factory is None:
            from core.provider_registry import create_realtime_provider
            provider_factory = create_realtime_provider
        self.factory = provider_factory
        self.asr = None
        self.partial_task = None
        self.response_task = None
        self.tts = None
        self.recording = False
        self.finishing = False
        self.pcm = bytearray()
        self.submitted = set()
        self.cancelled_turn = 0
        self.answer = ""
        self.committed_answer = None
        self.response_identity = None
        self.playback_identity = None

    async def emit(self, kind, **values):
        # 适配器可能一次返回很大的增量；控制帧须连同 JSON 转义后的内容，
        # 一起保持在设备 8192 字节解析上限以内。
        if kind == "llm.delta":
            import json
            value = values["text"]
            while value:
                end = min(len(value), 512)
                while (len(value[:end].encode("utf-8")) > 1024 or
                       len(json.dumps(value[:end]).encode("utf-8")) > 6000):
                    end //= 2
                await self.emit_callback({"type": kind, **self.session.identity(),
                                          "text": value[:end]})
                value = value[end:]
            return
        delivered = await self.emit_callback({"type": kind, **self.session.identity(), **values})
        if kind == "llm.done" and delivered is False:
            # 提交等待写锁时，传输层可能已封闭本轮；
            # 被丢弃的提交不能再启动新的 TTS 音频流。
            raise asyncio.CancelledError()

    async def start_utterance(self, turn):
        """建立单轮识别；流式 ASR 边收边识别，批量 ASR 留到录音结束调用。"""
        if turn <= self.cancelled_turn:
            raise ValueError("A canceled utterance cannot be restarted")
        if self.recording or self.finishing:
            raise ValueError("An utterance is already recording")
        await self.cancel_response()
        await self._close_asr()
        self.session.begin_turn(turn)
        self.recording = True
        self.pcm.clear()
        entry = self.session.selection.asr
        try:
            self.asr = self.factory("asr", entry, self.credentials, self.http)
            if entry.adapter != "qwen_batch":
                await self.asr.start()
                self.partial_task = asyncio.create_task(self._partials(self.session.turn))
        except BaseException:
            self.recording = False
            await self._close_asr()
            raise
        await self.emit("speech.started")

    async def _partials(self, turn):
        try:
            async for event in self.asr.events():
                if turn != self.session.turn or (self.cancelled_turn and turn <= self.cancelled_turn):
                    return
                if event.kind in ("partial", "segment_final"):
                    await self.emit("asr." + event.kind, text=event.text)
        except asyncio.CancelledError:
            raise
        except Exception:
            if turn == self.session.turn and (not self.cancelled_turn or turn > self.cancelled_turn):
                await self.emit("error", code="asr_events_failed")

    async def feed_audio(self, pcm):
        if not self.recording:
            return  # 显式发言之外的静音仍会推进编解码器状态。
        if len(pcm) % 2:
            raise ValueError("Invalid PCM16 samples")
        if self.session.selection.asr.adapter == "qwen_batch":
            if len(self.pcm) + len(pcm) > 960000:
                raise ValueError("Utterance exceeds 30 seconds")
            self.pcm.extend(pcm)
        else:
            await self.asr.feed_audio(pcm)

    async def end_utterance(self):
        """提交一次最终识别结果，空录音不调用 LLM 或 TTS。"""
        if not self.recording:
            return
        self.recording = False
        self.finishing = True
        turn = self.session.turn
        try:
            if self.session.selection.asr.adapter == "qwen_batch":
                # 限制单轮录音大小，同一连接每轮只发起一次识别。
                text = await self.asr.recognize_wav_async(pcm16_to_wav(bytes(self.pcm)), self.http)
            else:
                text = await self.asr.end_utterance()
        finally:
            self.pcm.clear()
            await self._close_asr()
            self.finishing = False
        if turn in self.submitted or turn <= self.cancelled_turn:
            return
        self.submitted = {turn}  # 回合编号递增，无需保留无限增长的去重集合。
        await self.emit("asr.utterance_final", text=text)
        if not text.strip():
            await self.emit("response.done", reason="empty_utterance")
            return
        self.session.add_history("user", text)
        self.session.generation += 1
        self.response_identity = self.session.identity()
        self.playback_identity = None
        self.response_task = asyncio.create_task(self._response())

    def seal_utterance(self, turn):
        """在取消操作等待服务商清理前，先阻止旧回调提交结果。"""
        self.cancelled_turn = max(self.cancelled_turn, turn)
        if turn == self.session.turn:
            self.recording = False

    async def cancel_utterance(self, turn):
        """丢弃本轮识别，不提交中间文字或空的最终结果。

        传输层先停止按序执行的输入与结束操作。排队中的开始请求可能在
        调用 begin_turn 前被取消，因此这里也要消耗该编号，确保下一轮递增。
        """
        self.seal_utterance(turn)
        await self.cancel_response()
        await self._close_asr()
        self.finishing = False
        self.pcm.clear()
        if self.session.turn < turn:
            self.session.begin_turn(turn)

    async def _response(self):
        identity = self.response_identity
        self.answer = ""
        self.committed_answer = None
        interrupted = False
        audio_task = None
        llm_stream = None
        tts = None
        tts_failed = False
        media = None
        segmenter = TextSegmenter()
        spoken_segments = []
        try:
            async with asyncio.timeout(120):
                # 在合成或播放前，先提交稳定的完整显示正文。
                # 增量仍提供给旧客户端与诊断；
                # 当前设备缓存增量，直到收到 llm.done 标记。
                await self.emit("response.started", text_mode="complete_before_audio")
                query = self.session.latest_user_text()
                direct_answer = deterministic_date_answer(query)
                messages = self.session.messages()
                identity_fields = self.session.identity()
                news_options = getattr(self.session.selection, "news", {})
                wants_live_search = needs_realtime_search(query)
                if news_options.get("enabled", False) and is_news_discussion(query):
                    wants_live_search = False
                search_enabled = bool(getattr(self.session.selection.llm, "options", {}).get(
                    "enable_web_search", False))
                news_request_info = news_request(query, news_options)
                wants_live_search = wants_live_search or news_request_info is not None
                news_status = news_credential_status(self.session.selection, self.credentials)
                if direct_answer is not None:
                    _LOG.info("request_route epoch=%d turn=%d generation=%d route=date search_enabled=%s",
                              identity_fields["session_epoch"], identity_fields["turn_id"],
                              identity_fields["generation"], search_enabled)
                elif wants_live_search:
                    selected_route = ("news" if news_request_info is not None and news_options.get("enabled", False)
                                      else "search")
                    _LOG.info("request_route epoch=%d turn=%d generation=%d route=%s search_enabled=%s news_enabled=%s",
                              identity_fields["session_epoch"], identity_fields["turn_id"],
                              identity_fields["generation"], selected_route, search_enabled,
                              news_options.get("enabled", False))
                else:
                    _LOG.info("request_route epoch=%d turn=%d generation=%d route=chat search_enabled=%s news_enabled=%s",
                              identity_fields["session_epoch"], identity_fields["turn_id"],
                              identity_fields["generation"], search_enabled,
                              news_options.get("enabled", False))
                if direct_answer is None and wants_live_search:
                    if (news_request_info is not None and news_options.get("enabled", False)
                            and news_status == "ready"):
                        _LOG.info("showapi_news status=pending kind=%s", news_request_info.kind)
                        try:
                            provider = ShowApiNewsProvider(news_options, self.credentials)
                            if news_request_info.kind == "channels":
                                items = await provider.channels(self.http)
                                direct_answer = "可用新闻频道包括：" + "、".join(item["name"] for item in items[:20])
                            elif news_request_info.kind == "hot":
                                items = await provider.hot(self.http)
                                if items:
                                    messages = with_news_results(messages, items)
                                    _LOG.info("showapi_news status=ok kind=hot results=%d", len(items))
                                else:
                                    direct_answer = "新闻平台暂时没有返回可播报的热搜内容。"
                            else:
                                items = await provider.search(
                                    news_request_info.keyword, self.http,
                                    channel_name=news_request_info.channel_name,
                                    begin_time=news_request_info.begin_time,
                                    end_time=news_request_info.end_time,
                                )
                                if items:
                                    messages = with_news_results(messages, items)
                                    _LOG.info("showapi_news status=ok kind=articles results=%d", len(items))
                                else:
                                    direct_answer = "新闻平台暂时没有返回相关新闻。"
                        except ShowApiNewsError as exc:
                            direct_answer = "新闻服务暂时无法提供结果，我不能确认最新消息，请稍后再试。"
                            _LOG.warning("showapi_news status=unavailable reason=%s http_status=%d upstream_code=%d",
                                         exc.reason, exc.http_status, exc.upstream_code)
                        except Exception:
                            direct_answer = "新闻服务暂时不可用，我不能确认最新消息，请稍后再试。"
                            _LOG.warning("showapi_news status=unavailable reason=internal_error http_status=0 upstream_code=0")
                    elif news_request_info is not None and news_options.get("enabled", False) and news_status != "ready":
                        direct_answer = "ShowAPI 新闻功能已开启，但尚未配置有效 AppKey。请先填写 SHOWAPI_APP_KEY。"
                        _LOG.info("showapi_news status=missing_credentials results=0")
                    elif not search_enabled:
                        direct_answer = "当前未启用联网搜索，我暂时无法核实这条实时信息。"
                        _LOG.info("web_search status=disabled results=0")
                    else:
                        _LOG.info("web_search status=pending")
                        try:
                            results = await AnySearchProvider(self.credentials).search(search_query(query), self.http)
                            messages = with_search_results(messages, results)
                            _LOG.info("web_search status=ok results=%d", len(results))
                        except AnySearchError as exc:
                            direct_answer = "这次联网搜索没有取得可核实的结果，暂时无法确认最新情况，请稍后再试。"
                            _LOG.warning("web_search status=unavailable reason=%s http_status=%d results=0",
                                         exc.reason, exc.http_status)
                        except Exception:
                            direct_answer = "这次联网搜索暂时不可用，我无法核实最新情况，请稍后再试。"
                            _LOG.warning("web_search status=unavailable reason=internal_error http_status=0 results=0")
                if direct_answer is not None:
                    self.answer = direct_answer
                    await self.emit("llm.delta", text=direct_answer)
                    spoken_segments.extend(segmenter.feed(direct_answer))
                else:
                    llm = self.factory("llm", self.session.selection.llm, self.credentials, self.http)
                    llm_stream = llm.stream(messages, self.http)
                    async for text in llm_stream:
                        if len(self.answer) + len(text) > 16000:
                            raise ValueError("Answer limit reached")
                        self.answer += text
                        await self.emit("llm.delta", text=text)
                        spoken_segments.extend(segmenter.feed(text))
                spoken_segments.extend(segmenter.finish())
                await self.emit("llm.done")
                self.committed_answer = self.answer
                try:
                    self.tts = tts = self.factory("tts", self.session.selection.tts, self.credentials, self.http)
                    await self.tts.start()
                    subtitle_enabled = bool(getattr(self.tts, "subtitle_enabled", False))
                    media = self.tts.media() if hasattr(self.tts, "media") else self.tts.audio()
                except Exception:
                    tts_failed = True
                    await self.emit("error", code="tts_failed")
                if media is not None and not tts_failed:
                    try:
                        audio_task = asyncio.create_task(self.send_audio(
                            media, identity, subtitle_enabled=subtitle_enabled))
                        for segment in spoken_segments:
                            if audio_task.done() and (audio_task.cancelled() or audio_task.exception() is not None):
                                raise RuntimeError("Audio transport failed")
                            await self.tts.append_text(segment)
                        await self.tts.finish()
                        # 清理流程负责取消并等待适配器迭代器退出。
                        await asyncio.shield(audio_task)
                    except Exception:
                        tts_failed = True
                        await self.emit("error", code="tts_failed")
                await self.emit("response.done", reason="tts_failed" if tts_failed else "complete")
        except asyncio.CancelledError:
            interrupted = True
            raise
        except Exception:
            interrupted = True
            await self.emit("error", code="response_failed")
        finally:
            record = None
            if self.answer:
                # 过长的问题加回答可能被整对淘汰；资源清理不能依赖缓存非空。
                record = self.session.add_history("assistant", self.answer, interrupted=interrupted)
                if self.playback_identity == identity:
                    record["playback_confirmed"] = True

            async def cleanup():
                if audio_task:
                    audio_task.cancel()
                # 适配器的音频迭代器可能在 finally 中关闭 WebSocket。
                # 等待迭代器退出前先进入取消状态，
                # 避免正常远端关闭握手耗尽 D2 时间预算。
                if interrupted and tts:
                    with suppress(Exception):
                        await tts.cancel()
                if audio_task:
                    with suppress(asyncio.CancelledError, Exception):
                        await audio_task
                # LLM 生成器暂停在 yield 时，取消可能中断循环体。
                # 在确认取消或启动下一轮之前，
                # 先在这里关闭生成器的 HTTP 上下文。
                if llm_stream is not None and hasattr(llm_stream, "aclose"):
                    with suppress(Exception):
                        await llm_stream.aclose()
                if tts:
                    with suppress(Exception):
                        await tts.close()

            # response.done 之后，上游连接仍可能在关闭，此时新一轮也能取消本轮。
            # 须先完成关闭，再复用状态。
            cleanup_task = asyncio.create_task(cleanup())
            try:
                await asyncio.shield(cleanup_task)
            except asyncio.CancelledError:
                if record and not record["playback_confirmed"]:
                    record["interrupted"] = True
                # response.done 可能早于最后一个上游连接关闭；此时插话须打断
                # 正常关闭握手，但仍需等清理结束，
                # 才能确认取消或复用状态。
                if tts:
                    with suppress(Exception):
                        await tts.cancel()
                await cleanup_task
                raise
            finally:
                if self.tts is tts:
                    self.tts = None

    def confirm_playback(self, identity):
        if identity != self.response_identity:
            return
        self.playback_identity = dict(identity)
        if self.session.history and self.session.history[-1]["role"] == "assistant":
            last = self.session.history[-1]
            if not last["interrupted"]:
                last["playback_confirmed"] = True

    async def cancel_response(self):
        """等待旧回答退出；未获播放确认的历史内容标记为被打断。"""
        task = self.response_task
        cancelled = bool(task and not task.done())
        if cancelled:
            task.cancel()
            with suppress(asyncio.CancelledError, Exception):
                await task
            await self.emit_callback({"type": "response.cancelled", **(self.response_identity or self.session.identity())})
        self.response_task = None
        if self.session.history and self.session.history[-1]["role"] == "assistant":
            last = self.session.history[-1]
            if not last["playback_confirmed"]:
                last["interrupted"] = True
        return cancelled

    async def _close_asr(self):
        partial_task, self.partial_task = self.partial_task, None
        asr, self.asr = self.asr, None
        if partial_task:
            partial_task.cancel()
            with suppress(asyncio.CancelledError, Exception):
                await partial_task
        if asr and hasattr(asr, "close"):
            with suppress(Exception):
                await asr.close()

    async def close(self):
        await self.cancel_response()
        await self._close_asr()
        self.recording = False
        self.finishing = False
        self.pcm.clear()
