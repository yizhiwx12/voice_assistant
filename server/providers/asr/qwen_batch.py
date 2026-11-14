"""千问批量 ASR：收齐单轮 16 kHz 单声道 WAV 后一次识别。

该接口没有实时中间结果；模型和接口地址取自配置，真实密钥只通过
credential_ref 读取，不能放到 ESP32 固件或示例配置中。
"""

import base64
import asyncio
import json
import re
from collections.abc import Mapping
from urllib.request import Request, urlopen

from core.text_utils import clean_single_line
from providers._http import request_json, require_credential


class QwenBatchProvider:
    capabilities = {
        "streaming_input": False,
        "streaming_output": False,
        "partial_results": False,
    }

    def __init__(self, options: Mapping, credentials: Mapping, opener=None):
        self.options = options
        self.credentials = credentials
        self.opener = opener if opener is not None else urlopen

    def _request_parts(self, wav_data):
        api_key = require_credential(
            self.credentials, self.options.get("credential_ref", "DASHSCOPE_API_KEY")
        )
        endpoint = self.options.get("endpoint")
        if not endpoint:
            workspace = require_credential(
                self.credentials,
                self.options.get("workspace_ref", "DASHSCOPE_WORKSPACE_ID"),
            )
            if not re.fullmatch(r"[A-Za-z0-9-]+", workspace):
                raise RuntimeError("Qwen ASR workspace ID has an invalid format")
            endpoint = (
                f"https://{workspace}.cn-beijing.maas.aliyuncs.com"
                "/api/v1/services/aigc/multimodal-generation/generation"
            )

        audio_data = base64.b64encode(wav_data).decode("ascii")
        payload = {
            "model": self.options.get("model", "qwen-audio-3.1-asr-flash"),
            "input": {
                "messages": [{
                    "role": "user",
                    "content": [{
                        "type": "input_audio",
                        "input_audio": {"data": f"data:audio/wav;base64,{audio_data}"},
                    }],
                }],
            },
            "parameters": {
                "format": "wav",
                "sample_rate": "16000",
                "language_hints": list(self.options.get("language_hints", ("zh",))),
            },
        }
        headers = {
                "Authorization": f"Bearer {api_key}",
                "Content-Type": "application/json",
                "X-DashScope-SSE": "disable",
        }
        return endpoint, payload, headers

    def recognize_wav(self, wav_data: bytes) -> str:
        endpoint, payload, headers = self._request_parts(wav_data)
        request = Request(
            endpoint,
            data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
            headers=headers,
            method="POST",
        )
        result = request_json(
            request,
            opener=self.opener,
            timeout=self.options.get("timeout_s", 60),
            label="Qwen ASR",
        )
        return self._transcript(result)

    async def recognize_wav_async(self, wav_data, session):
        """供异步实时协调器使用的可取消批量识别请求。"""
        import aiohttp

        endpoint, payload, headers = self._request_parts(wav_data)
        try:
            async with session.post(
                endpoint, json=payload, headers=headers,
                timeout=aiohttp.ClientTimeout(total=self.options.get("timeout_s", 60)),
            ) as response:
                if response.status != 200:
                    raise RuntimeError(f"Qwen ASR HTTP {response.status}")
                try:
                    result = json.loads(await response.read())
                except (UnicodeDecodeError, json.JSONDecodeError):
                    raise RuntimeError("Qwen ASR returned invalid JSON") from None
        except (aiohttp.ClientError, asyncio.TimeoutError):
            raise RuntimeError("Qwen ASR connection failed") from None
        return self._transcript(result)

    @staticmethod
    def _transcript(result):
        try:
            transcript = result["output"]["text"]
        except (KeyError, TypeError):
            raise RuntimeError("Qwen ASR returned an unexpected response") from None
        if not isinstance(transcript, str):
            raise RuntimeError("Qwen ASR returned non-text content")
        return clean_single_line(transcript)
