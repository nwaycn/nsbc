"""nsbc_ai — NSBC 外部 AI Agent 生产级包

架构分层（对齐 PROTOCOL.md §4）：
  nsbc_ai.client          → NSBC media API（HTTP polling / WebSocket Binary frame）
  nsbc_ai.vad             → BargeInController（VAD-based 用户打断）
  nsbc_ai.agent           → Agent 主循环 + LLM function call dispatch
  nsbc_ai.providers.base  → ASRProvider / LLMProvider / TTSProvider 抽象接口
  nsbc_ai.providers.mock  → MockASR / MockLLM / MockTTS（纯关键词检测，零 API key）
  nsbc_ai.providers.whisper    → Whisper ASR（OpenAI audio/transcriptions REST）
  nsbc_ai.providers.elevenlabs → ElevenLabs TTS（text-to-speech REST → PCMU 8kHz）
  nsbc_ai.providers.ollama    → Ollama /api/chat LLM（本地/自建）

每个 provider 接口都是纯同步（stream=False），方便串行 AI 主循环。
"""
from .client import NsbcMediaClient, HttpTransport, WsTransport
from .vad import BargeInController
from .agent import AiAgent, AgentConfig
from .providers.base import ASRProvider, LLMProvider, TTSProvider, LLMReply

__all__ = [
    "NsbcMediaClient", "HttpTransport", "WsTransport",
    "BargeInController",
    "AiAgent", "AgentConfig",
    "ASRProvider", "LLMProvider", "TTSProvider", "LLMReply",
]
