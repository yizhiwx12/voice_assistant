"""两个服务入口共用的稳定 PCM HTTP 接口约定。"""

from http.server import BaseHTTPRequestHandler
from pathlib import Path

from audio.formats import pcm16_to_wav
from core.text_utils import clean_single_line, truncate_utf8

MAX_AUDIO_BYTES = 1_000_000
MAX_TRANSCRIPT_BYTES = 96
MAX_REPLY_BYTES = 120


def make_handler(recognize_wav, generate_reply, *, data_directory):
    """把服务商绑定到旧版 /ping 和 /audio 路由，不依赖全局配置。

    回调由入口显式解析；这样既能共用 HTTP 实现，也能保留旧入口供测试替换
    或嵌入其他程序使用的钩子。
    """
    data_directory = Path(data_directory)

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def setup(self):
            super().setup()
            self.connection.settimeout(30)

        def send_text(self, status, text):
            body = text.encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            self.send_text(200, "OK") if self.path == "/ping" else self.send_text(404, "Not found")

        def do_POST(self):
            if self.path != "/audio":
                self.send_text(404, "Not found")
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                self.send_text(400, "Invalid Content-Length")
                return
            if (self.headers.get("Transfer-Encoding") is not None
                    or len(self.headers.get_all("Content-Length", [])) != 1
                    or length <= 0 or length > MAX_AUDIO_BYTES or length % 2):
                self.send_text(400, "Invalid PCM16 audio length")
                return
            try:
                audio = self.rfile.read(length)
            except (TimeoutError, OSError):
                self.send_text(400, "Incomplete audio body")
                return
            if len(audio) != length:
                self.send_text(400, "Incomplete audio body")
                return

            try:
                transcript = clean_single_line(recognize_wav(pcm16_to_wav(audio)))
            except Exception:
                # 上游异常可能包含请求正文或凭证。
                print("ASR request failed", flush=True)
                self.send_text(500, "Speech recognition failed")
                return

            try:
                data_directory.mkdir(parents=True, exist_ok=True)
                (data_directory / "received_audio.pcm").write_bytes(audio)
            except OSError:
                print("Unable to save HTTP debug recording", flush=True)
                self.send_text(500, "Unable to save audio")
                return

            recognized_characters = len(transcript)
            if transcript:
                try:
                    reply = clean_single_line(generate_reply(transcript))
                    if not reply:
                        raise ValueError("Empty answer")
                except Exception:
                    print("LLM request failed", flush=True)
                    # 即使选中了其他 LLM，也保留此兼容响应。
                    self.send_text(502, "DeepSeek request failed")
                    return
            else:
                transcript = "未识别到语音"
                reply = "请靠近麦克风后重试。"

            print(f"Received {len(audio)} bytes; recognized {recognized_characters} characters", flush=True)
            self.send_text(200, f"{truncate_utf8(transcript, MAX_TRANSCRIPT_BYTES)}\n"
                                f"{truncate_utf8(reply, MAX_REPLY_BYTES)}")

    return Handler
