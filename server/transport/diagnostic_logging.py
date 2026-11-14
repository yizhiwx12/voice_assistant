"""有容量上限的诊断输出，避免在事件循环中等待终端。

只有专用的固定字段语音诊断日志使用此处理器。写线程执行输出时不持有
日志处理器锁，Windows 控制台即使暂停，也不能让 logging.shutdown() 无限等待。
"""

from dataclasses import dataclass
import logging
import queue
import sys
import threading


DIAGNOSTIC_FORMAT = "%(asctime)s %(levelname)s %(name)s %(message)s"


@dataclass(frozen=True)
class DiagnosticLogStats:
    queued: int
    capacity: int
    accepted: int
    written: int
    dropped: int
    sink_failures: int
    accepting: bool
    writer_alive: bool


class BoundedDiagnosticHandler(logging.Handler):
    """生成短小且不可变的日志行，入队时不等待输出空间。

    队列容量和行长度共同限制保留的输出，即使输出流一直未恢复也不会无限增长。
    队列满或输出失败时不调用可能阻塞 stderr 的 logging.handleError()。
    输出流归调用者所有，本处理器不关闭它。
    """

    def __init__(self, stream=None, *, capacity=128, max_line_chars=4096,
                 stop_timeout=0.5):
        if type(capacity) is not int or capacity < 1:
            raise ValueError("Diagnostic queue capacity must be positive")
        if type(max_line_chars) is not int or max_line_chars < 128:
            raise ValueError("Diagnostic line limit must be at least 128")
        if not 0 <= stop_timeout <= 5:
            raise ValueError("Diagnostic stop timeout must be between 0 and 5 seconds")
        super().__init__()
        self.setFormatter(logging.Formatter(DIAGNOSTIC_FORMAT))
        self._stream = sys.stderr if stream is None else stream
        self._queue = queue.Queue(maxsize=capacity)
        self._line_limit = max_line_chars
        self._stop_timeout = stop_timeout
        self._state_lock = threading.Lock()
        self._stopping = threading.Event()
        self._accepting = True
        self._accepted = 0
        self._written = 0
        self._dropped = 0
        self._pending_drops = 0
        self._sink_failures = 0
        self._writer = threading.Thread(target=self._run, name="voice_diagnostics", daemon=True)
        self._writer.start()

    def _drop(self, *, sink_failure=False):
        with self._state_lock:
            self._dropped += 1
            self._pending_drops += 1
            if sink_failure:
                self._sink_failures += 1

    def emit(self, record):
        # 格式化本地数值与字符串字段不会进行流输出。
        # 队列中不保留调用者参数、异常回溯或 LogRecord。
        try:
            line = self.format(record)
            if len(line) > self._line_limit:
                line = line[:self._line_limit - 12] + " [truncated]"
        except Exception:
            self._drop()
            return
        with self._state_lock:
            if not self._accepting:
                return
            try:
                self._queue.put_nowait(line)
            except queue.Full:
                self._dropped += 1
                self._pending_drops += 1
            else:
                self._accepted += 1

    def _write(self, line):
        # 执行可能阻塞的调用时，不持有处理器、状态或队列锁；
        # 特别要保证 logging.shutdown 仍能获取 Handler.lock。
        self._stream.write(line + "\n")
        self._stream.flush()

    def _report_drops(self):
        with self._state_lock:
            pending, total = self._pending_drops, self._dropped
            self._pending_drops = 0
        if not pending:
            return
        try:
            record = logging.LogRecord("transport.voice_websocket", logging.WARNING,
                                       __file__, 0,
                                       "diagnostic_log_dropped count=%d total=%d",
                                       (pending, total), None)
            self._write(self.format(record))
        except Exception:
            with self._state_lock:
                self._pending_drops += pending
                self._sink_failures += 1

    def _run(self):
        while not self._stopping.is_set() or not self._queue.empty():
            try:
                line = self._queue.get(timeout=0.05)
            except queue.Empty:
                self._report_drops()
                continue
            try:
                self._write(line)
            except Exception:
                self._drop(sink_failure=True)
            else:
                with self._state_lock:
                    self._written += 1
                self._report_drops()
            finally:
                self._queue.task_done()
        self._report_drops()

    def stats(self):
        with self._state_lock:
            return DiagnosticLogStats(self._queue.qsize(), self._queue.maxsize,
                                      self._accepted, self._written, self._dropped,
                                      self._sink_failures, self._accepting,
                                      self._writer.is_alive())

    def stop(self, timeout=None):
        """停止接收日志，最多等待 timeout 秒排空队列。

        调用者的输出流阻塞时返回 False，由守护线程保留有界队列。
        输出流恢复后可完成剩余输出，但不能阻止本处理器清理。
        底层流及解释器退出时的最终刷新行为仍由调用者负责。
        """
        timeout = self._stop_timeout if timeout is None else timeout
        if not 0 <= timeout <= 5:
            raise ValueError("Diagnostic stop timeout must be between 0 and 5 seconds")
        with self._state_lock:
            self._accepting = False
        self._stopping.set()
        if threading.current_thread() is not self._writer and self._writer.ident is not None:
            self._writer.join(timeout)
        return not self._writer.is_alive()

    def close(self):
        self.stop()
        super().close()
