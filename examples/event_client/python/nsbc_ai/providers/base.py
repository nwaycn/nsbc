"""Provider 抽象基类 — 所有 ASR/LLM/TTS 实现必须对齐此接口

设计原则（来自 experience 1280962 Provider 抽象经验）：
  - 每个 Provider 只管一件事（ASR / LLM / TTS），接口独立
  - 纯同步方法，方便串行 AI 主循环调用
  - 零硬编码密钥 —— 通过 __init__ 参数传入（api_key / endpoint / model）
  - Mock 实现和真实实现签名完全相同，可以无缝替换
"""
from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass
from typing import Optional


# =============================================================================
# LLM function call 结构化回复（单一事实源）
# =============================================================================

@dataclass
class LLMReply:
    """LLM 对 ASR transcript 的结构化回复。

    action 字段：
      "say"       → 文本回复 → TTS → 用户听
      "transfer"  → ai_request_transfer(dest) — 转人工座席
      "hangup"    → uuid_kill — 挂断通话
      "silence"   → 不回复（用户没说话 / barge-in 中 / LLM 返回空）
    """
    action: str                                    # "say" | "transfer" | "hangup" | "silence"
    text: Optional[str] = None                     # action="say" 时的回复文本
    dest: Optional[str] = None                     # action="transfer" 时的转接目标（分机号或 SIP URI）
    reason: Optional[str] = None                   # 可选，触发原因（日志用）

    # ---- 工厂方法 ----
    @classmethod
    def say(cls, text: str, reason: str = "") -> "LLMReply":
        return cls(action="say", text=text, reason=reason)

    @classmethod
    def transfer(cls, dest: str, reason: str = "") -> "LLMReply":
        return cls(action="transfer", dest=dest, reason=reason)

    @classmethod
    def hangup(cls, reason: str = "") -> "LLMReply":
        return cls(action="hangup", reason=reason)

    @classmethod
    def silence(cls, reason: str = "") -> "LLMReply":
        return cls(action="silence", reason=reason)


# =============================================================================
# ASR — 语音 → 文字
# =============================================================================

class ASRProvider(ABC):
    """ASR 接口：接受 PCMU 8kHz 原始字节流，返回识别文本。

    为什么输入是 PCMU 而不是 WAV：nsbc_ai 收到的媒体就是 PCMU 8kHz（ai_get_rx / WS Binary frame），
    直接透传给 ASR 服务。真实 Provider 内部负责转成服务端要求的格式（multipart WAV / raw PCM16 base64 / ...）。"""

    @abstractmethod
    def transcribe(self, pcmu_bytes: bytes) -> str:
        """识别 PCMU 8kHz 原始字节。空输入返回 ""。有音频但识别失败也返回 ""（让上层当静音处理）。"""
        ...


# =============================================================================
# LLM — 文字 → 结构化回复
# =============================================================================

class LLMProvider(ABC):
    """LLM 接口：接受 ASR transcript，返回结构化 LLMReply。

    为什么返回 LLMReply 而不是原始 JSON：Agent 主循环需要明确的 action 分支（say / transfer / hangup），
    把 JSON parse + action 解析封装在 LLMProvider 内部，Agent 只管 dispatch。"""

    @abstractmethod
    def chat(self, transcript: str) -> LLMReply:
        """对 transcript 做 LLM 推理。返回 LLMReply。"""
        ...


# =============================================================================
# TTS — 文字 → PCMU 8kHz 音频
# =============================================================================

class TTSProvider(ABC):
    """TTS 接口：接受文本，返回 PCMU 8kHz 原始字节。

    为什么输出 PCMU 而不是 PCM16：nsbc_ai 推给 ai_send_tx_audio / WS Binary frame 的就是 PCMU，
    真实 Provider 内部负责服务端格式（PCM16 / MP3 / ...）→ PCMU 8kHz 转换。"""

    @abstractmethod
    def synthesize(self, text: str) -> bytes:
        """把文本合成 PCMU 8kHz 音频。空文本返回 b""。"""
        ...
