#!/usr/bin/env python3
"""ai_agent.py — 外部 AI Agent 入口（向后兼容旧命令行 + 真实云服务 Provider）

本脚本是 nsbc_ai 包的薄入口。所有生产逻辑在 examples/event_client/python/nsbc_ai/ 包内。

旧命令行（继续可用）：
  python3 ai_agent.py --api-base https://nsbc-web.local:8080 \\
      --api-token <Admin-ext-key> --call-id <call_id> \\
      --transport auto --vad-barge-in --barge-in-rms 800

新功能：--asr / --llm / --tts 选择真实 Provider
  python3 ai_agent.py --asr whisper --llm ollama --tts elevenlabs \\
      --api-key-whisper sk-xxx --api-key-llm ollama --api-key-tts sk-xxx

  python3 ai_agent.py --asr whisper --tts elevenlabs --llm mock \\
      --llm-transfer-dest 2001   # LLM 不指定时用这个

Provider 列表：
  asr:   mock | whisper（OpenAI / 兼容 audio/transcriptions）
  llm:   mock | ollama（本地/自建，OpenAI Chat 兼容端点也能用）
  tts:   mock | elevenlabs

真实 Provider 环境变量（也可用命令行参数）：
  NSBC_WHISPER_API_KEY  / --api-key-whisper
  NSBC_OLLAMA_ENDPOINT  / --llm-endpoint  (默认 http://localhost:11434/api/chat)
  NSBC_ELEVENLABS_API_KEY / --api-key-tts
  NSBC_ELEVENLABS_VOICE_ID / --tts-voice-id
"""
from __future__ import annotations

import argparse
import os
import sys

# 让脚本目录下的 nsbc_ai 包可 import
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from nsbc_ai import AgentConfig, AiAgent
from nsbc_ai.providers.base import ASRProvider, LLMProvider, TTSProvider


# =============================================================================
# Provider 工厂 —— 命令行 arg → 真实 Provider 实例
# =============================================================================

def build_asr(name: str, api_key: str, **kw) -> ASRProvider:
    if name == "mock":
        from nsbc_ai.providers.mock import MockASR
        return MockASR()
    if name == "whisper":
        from nsbc_ai.providers.whisper import WhisperASR
        return WhisperASR(api_key=api_key,
                          endpoint=kw.get("endpoint", "https://api.openai.com/v1/audio/transcriptions"),
                          model=kw.get("model", "whisper-1"))
    raise ValueError(f"Unknown ASR: {name}")


def build_llm(name: str, transfer_dest: str, **kw) -> LLMProvider:
    if name == "mock":
        from nsbc_ai.providers.mock import MockLLM
        return MockLLM(transfer_dest=transfer_dest)
    if name in ("ollama", "openai_chat", "deepseek"):
        from nsbc_ai.providers.ollama import OllamaLLM
        return OllamaLLM(model=kw.get("model", "llama3.1:8b"),
                         endpoint=kw.get("endpoint", "http://localhost:11434/api/chat"),
                         api_key=kw.get("api_key", ""),
                         transfer_dest=transfer_dest)
    raise ValueError(f"Unknown LLM: {name}")


def build_tts(name: str, api_key: str, **kw) -> TTSProvider:
    if name == "mock":
        from nsbc_ai.providers.mock import MockTTS
        return MockTTS()
    if name == "elevenlabs":
        from nsbc_ai.providers.elevenlabs import ElevenLabsTTS
        return ElevenLabsTTS(api_key=api_key,
                             voice_id=kw.get("voice_id", "21m00Tcm4TlvDq8ikWAM"))
    raise ValueError(f"Unknown TTS: {name}")


# =============================================================================
# main
# =============================================================================

def main():
    p = argparse.ArgumentParser(description="NSBC External AI Agent")

    # 旧参数（全保留）
    p.add_argument("--api-base", default="https://nsbc-web.local:8080")
    p.add_argument("--api-token", required=True, help="Admin scope ext_key")
    p.add_argument("--call-id", required=True, help="从 SSE CALL_ANSWERED 事件拿到的 call_id")
    p.add_argument("--transfer-dest", default="1001", help="transfer 默认目标")
    p.add_argument("--transport", choices=["http", "ws", "auto"], default="auto")
    p.add_argument("--vad-barge-in", dest="barge_in", action="store_true", default=True)
    p.add_argument("--no-vad-barge-in", dest="barge_in", action="store_false")
    p.add_argument("--barge-in-rms", type=int, default=800)
    p.add_argument("--chunk-bytes", type=int, default=160)

    # 新参数：Provider 选择
    p.add_argument("--asr", choices=["mock", "whisper"], default=os.environ.get("NSBC_ASR", "mock"))
    p.add_argument("--llm", choices=["mock", "ollama", "openai_chat", "deepseek"],
                   default=os.environ.get("NSBC_LLM", "mock"))
    p.add_argument("--tts", choices=["mock", "elevenlabs"], default=os.environ.get("NSBC_TTS", "mock"))

    # Provider 配置
    p.add_argument("--api-key-whisper", default=os.environ.get("NSBC_WHISPER_API_KEY", ""))
    p.add_argument("--llm-endpoint", default=os.environ.get("NSBC_OLLAMA_ENDPOINT", "http://localhost:11434/api/chat"))
    p.add_argument("--llm-model", default="llama3.1:8b")
    p.add_argument("--api-key-llm", default=os.environ.get("NSBC_OLLAMA_API_KEY", ""))
    p.add_argument("--api-key-tts", default=os.environ.get("NSBC_ELEVENLABS_API_KEY", ""))
    p.add_argument("--tts-voice-id", default=os.environ.get("NSBC_ELEVENLABS_VOICE_ID", "21m00Tcm4TlvDq8ikWAM"))
    p.add_argument("--whisper-endpoint", default="https://api.openai.com/v1/audio/transcriptions")

    args = p.parse_args()

    # --- config ---
    cfg = AgentConfig(
        api_base=args.api_base,
        api_token=args.api_token,
        call_id=args.call_id,
        transport=args.transport,
        chunk_bytes=args.chunk_bytes,
        barge_in_enabled=args.barge_in,
        barge_in_rms=args.barge_in_rms,
        transfer_dest=args.transfer_dest,
    )

    # --- providers ---
    try:
        asr = build_asr(args.asr, args.api_key_whisper, endpoint=args.whisper_endpoint)
        llm = build_llm(args.llm, args.transfer_dest,
                        endpoint=args.llm_endpoint, model=args.llm_model, api_key=args.api_key_llm)
        tts = build_tts(args.tts, args.api_key_tts, voice_id=args.tts_voice_id)
    except ValueError as e:
        print(f"[ai_agent] Provider 配置错误: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"[ai_agent] ASR={args.asr}  LLM={args.llm}  TTS={args.tts}")

    # --- build agent + run ---
    agent = AiAgent.from_config(cfg, asr=asr, llm=llm, tts=tts)
    agent.run()


if __name__ == "__main__":
    main()
