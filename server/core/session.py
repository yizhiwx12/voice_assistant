"""保存连接身份、回合编号和有限对话历史。

epoch 区分重连，turn 区分用户发言，generation 区分生成结果；设备用这组
身份丢弃过期消息。播放确认之前不假定用户已听完整段回答。
"""

import secrets

from core.realtime_info import current_time_context


class ConversationSession:
    def __init__(self, selection):
        self.selection = selection
        self.epoch = secrets.randbelow(0xFFFFFFFE) + 1
        self.turn = 0
        self.generation = 0
        self.history = []

    def begin_turn(self, turn):
        if type(turn) is not int or not self.turn < turn <= 0xFFFFFFFF:
            raise ValueError("turn_id must increase monotonically")
        self.turn = turn

    def identity(self):
        return {"session_epoch": self.epoch, "turn_id": self.turn,
                "generation": self.generation}

    def add_history(self, role, text, *, interrupted=False):
        """返回本次记录；即使超出缓存预算被淘汰，调用者仍可完成状态清理。"""
        record = {"role": role, "content": text,
                  "interrupted": interrupted, "playback_confirmed": False}
        self.history.append(record)
        # 尽量保留完整的用户提问与助手回答对。
        while len(self.history) > 12 or sum(len(item["content"]) for item in self.history) > 32000:
            self.history.pop(0)
        while self.history and self.history[0]["role"] != "user":
            self.history.pop(0)
        return record

    def messages(self):
        """每轮附上当前时间，并告知模型哪些历史回答可能没有被听完。"""
        result = [{"role": "system", "content": "你是桌面语音助手。请用简体中文自然、简短地回答，不要使用Markdown。"
                   + current_time_context()}]
        for item in self.history:
            content = item["content"]
            if item["role"] == "assistant" and item["interrupted"]:
                content += "\n[此回答被打断，用户听到的内容不确定。]"
            result.append({"role": item["role"], "content": content})
        return result

    def latest_user_text(self):
        return next((item["content"] for item in reversed(self.history)
                     if item["role"] == "user"), "")
