"""Mock ASR / LLM / TTS —— 零 API key，生产验证前用"""
from __future__ import annotations

import random
from typing import List

from .base import ASRProvider, LLMProvider, TTSProvider, LLMReply


DEFAULT_KEYWORDS_TRANSFER = ["转人工", "人工", "客服", "人工服务", "找人工", "转客服", "人工台",
                              "agent", "human", "operator", "representative"]
DEFAULT_KEYWORDS_HANGUP = ["挂断", "再见", "拜拜", "谢谢", "hangup", "bye"]


class MockASR(ASRProvider):
    """纯关键词检测 —— PCMU 有能量（>24）就返回预设词。"""

    def __init__(self, trigger_word: str = "我想转人工", energy_threshold: int = 24):
        self.trigger_word = trigger_word
        self.energy_threshold = energy_threshold

    def transcribe(self, pcmu_bytes: bytes) -> str:
        if not pcmu_bytes:
            return ""
        if any(b > self.energy_threshold for b in pcmu_bytes):
            return self.trigger_word
        return ""


class MockLLM(LLMProvider):
    """关键词匹配 → LLMReply。零 API 调用。"""

    def __init__(self,
                 transfer_keywords: List[str] | None = None,
                 hangup_keywords: List[str] | None = None,
                 transfer_dest: str = "1001"):
        self.transfer_keywords = transfer_keywords or DEFAULT_KEYWORDS_TRANSFER
        self.hangup_keywords = hangup_keywords or DEFAULT_KEYWORDS_HANGUP
        self.transfer_dest = transfer_dest

    def chat(self, transcript: str) -> LLMReply:
        if not transcript:
            return LLMReply.silence()

        lowered = transcript.lower()

        for kw in self.transfer_keywords:
            if kw.lower() in lowered:
                return LLMReply.transfer(dest=self.transfer_dest, reason=f"keyword={kw}")

        for kw in self.hangup_keywords:
            if kw.lower() in lowered:
                return LLMReply.hangup(reason=f"keyword={kw}")

        reply = f"我收到了您说的：{transcript[:40]}。请问还需要什么帮助吗？"
        return LLMReply.say(text=reply)


class MockTTS(TTSProvider):
    """返回静音 PCMU —— 零网络调用。生产替换成 ElevenLabsTTS。"""

    def __init__(self, duration_ms: int = 2000):
        """duration_ms: 返回多少毫秒静音。8kHz → 每 1ms 1 字节 PCMU。"""
        self.duration_ms = duration_ms

    def synthesize(self, text: str) -> bytes:
        if not text:
            return b""
        return bytes(self.duration_ms)  # 全 0 byte = 静音 PCMU
