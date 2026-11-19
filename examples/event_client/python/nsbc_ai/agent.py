"""AiAgent — AI agent 主循环 + LLM function call dispatch

架构（从 experience 1280962 / 2281619 吸取的教训）：
  - Agent 只管主循环和 dispatch，ASR/LLM/TTS 注入式传入（Provider 抽象）
  - Client（媒体 transport + 控制命令）也注入式传入
  - barge-in 作为可选组件注入（关掉就是纯媒体管道）
  - 任何 action="transfer" / action="hangup" 后 Agent 立刻 break + cleanup
  - 所有异常 catch + cleanup — 保证 producer=0 一定执行

dispatch 规则（单一事实源，和 Go agent 对称）：
  LLMReply.action == "say"       → tts.synthesize → client.send_tx
  LLMReply.action == "transfer"  → client.request_transfer(dest) → break
  LLMReply.action == "hangup"    → client.hangup() → break
  LLMReply.action == "silence"   → continue（等下一个 chunk）"""
from __future__ import annotations

import argparse
import sys
import time
from dataclasses import dataclass, field
from typing import Optional

from .client import NsbcMediaClient
from .vad import BargeInController
from .providers.base import ASRProvider, LLMProvider, TTSProvider, LLMReply


@dataclass
class AgentConfig:
    """Agent 运行时配置。所有 Provider 都可选注入（mock 默认值方便快速验证）。"""
    # transport
    api_base: str = "https://nsbc-web.local:8080"
    api_token: str = ""
    call_id: str = ""
    transport: str = "auto"                    # "http" | "ws" | "auto"

    # media
    chunk_bytes: int = 160                     # 每次 get_rx 拉多少字节（20ms PCMU）

    # barge-in
    barge_in_enabled: bool = True
    barge_in_rms: int = 800
    transfer_dest: str = "1001"                # LLM 没给 dest 时的默认 transfer 目标

    # welcome
    welcome_text: str = "您好，我是 AI 助理。请问需要什么帮助？"


class AiAgent:
    """AI agent 主循环。

    构造时注入 Client + 三个 Provider（ASR / LLM / TTS），运行 run() 进入主循环。
    任何 Provider 都可以是 mock（零 API key）或真实服务（ElevenLabs / Whisper / Ollama）。"""

    def __init__(self,
                 config: AgentConfig,
                 client: NsbcMediaClient,
                 asr: ASRProvider,
                 llm: LLMProvider,
                 tts: TTSProvider,
                 barge: Optional[BargeInController] = None):
        self.cfg = config
        self.client = client
        self.asr = asr
        self.llm = llm
        self.tts = tts
        self.barge = barge
        self._running = False
        self._stats = {"rx_bytes": 0, "asr_count": 0, "llm_count": 0, "tts_bytes": 0,
                       "transfers": 0, "hangups": 0, "cycles": 0}

    # =================================================================
    # 工厂方法：最简配置（mock Provider + auto transport）
    # =================================================================

    @classmethod
    def from_config(cls, cfg: AgentConfig, **provider_kwargs) -> "AiAgent":
        """方便入口。Provider 可通过 provider_kwargs 覆盖，默认 mock。"""
        from .providers.mock import MockASR, MockLLM, MockTTS

        client = NsbcMediaClient(cfg.api_base, cfg.api_token, cfg.call_id, cfg.transport)

        asr = provider_kwargs.get("asr") or MockASR()
        llm = provider_kwargs.get("llm") or MockLLM(transfer_dest=cfg.transfer_dest)
        tts = provider_kwargs.get("tts") or MockTTS()

        barge = BargeInController(client, cfg.barge_in_rms) if cfg.barge_in_enabled else None

        return cls(cfg=cfg, client=client, asr=asr, llm=llm, tts=tts, barge=barge)

    # =================================================================
    # 主循环
    # =================================================================

    def run(self) -> None:
        """进入主循环直到 transfer / hangup / Ctrl+C / 连接断开。"""
        self._running = True

        # 1. 连接 + producer=1
        if not self.client.connect():
            print("[agent] ❌ connect 失败，退出", file=sys.stderr)
            return

        print(f"\n{'=' * 70}")
        print(f"[agent] call={self.cfg.call_id}  transfer_dest={self.cfg.transfer_dest}")
        print(f"  barge_in={'on' if self.barge else 'off'}  chunk={self.cfg.chunk_bytes}")
        print(f"{'=' * 70}\n")

        # 2. 欢迎语
        welcome_pcmu = self.tts.synthesize(self.cfg.welcome_text)
        if welcome_pcmu:
            print(f"[agent] → TTS 欢迎语 ({len(welcome_pcmu)} bytes)")
            self.client.send_tx(welcome_pcmu)
            self._stats["tts_bytes"] += len(welcome_pcmu)

        print("[agent] 循环开始 — Ctrl+C 退出\n")

        try:
            while self._running:
                self._cycle()
        except KeyboardInterrupt:
            print("\n[agent] Ctrl+C 退出")
        finally:
            self._cleanup()

    def _cycle(self) -> None:
        """单次主循环迭代。返回后 run() 决定是否继续。"""
        self._stats["cycles"] += 1

        # --- 拉用户语音 ---
        rx = self.client.get_rx(self.cfg.chunk_bytes)
        if not rx:
            time.sleep(0.05)
            return

        self._stats["rx_bytes"] += len(rx)

        # --- barge-in: 等用户说完 ---
        if self.barge:
            self.barge.wait_until_silence(timeout_s=1.0)

        # --- ASR ---
        transcript = self.asr.transcribe(rx)
        if not transcript:
            return
        self._stats["asr_count"] += 1

        print(f"[agent] ASR: {transcript}")

        # --- LLM ---
        reply = self.llm.chat(transcript)
        self._stats["llm_count"] += 1

        self._dispatch(reply)

    def _dispatch(self, reply: LLMReply) -> None:
        """按 LLMReply.action dispatch 到对应分支。"""
        action = reply.action

        if action == "transfer":
            dest = reply.dest or self.cfg.transfer_dest
            print(f"[agent] ★ LLM → transfer dest={dest}  reason={reply.reason}", flush=True)
            self.client.request_transfer(dest)
            self._stats["transfers"] += 1
            self._running = False
            return

        if action == "hangup":
            print(f"[agent] ★ LLM → hangup  reason={reply.reason}", flush=True)
            self.client.hangup()
            self._stats["hangups"] += 1
            self._running = False
            return

        if action == "silence":
            return

        if action == "say":
            text = reply.text or ""
            if not text:
                return

            # barge-in: 说之前再等一次
            if self.barge:
                self.barge.wait_until_silence(timeout_s=1.0)

            print(f"[agent] LLM say: {text}")
            pcmu = self.tts.synthesize(text)
            if pcmu:
                ok = self.client.send_tx(pcmu)
                self._stats["tts_bytes"] += len(pcmu)
                if ok:
                    print(f"[agent] ← TTS 回复 ({len(pcmu)} bytes)")
                else:
                    print(f"[agent] ← TTS 发送失败！", file=sys.stderr)
            return

        # 未知 action → 跳过
        print(f"[agent] 未知 action: {action}", file=sys.stderr)

    # =================================================================
    # 生命周期
    # =================================================================

    def _cleanup(self) -> None:
        """幂等清理 — 任何退出路径都走这里，保证 producer=0 一定执行。"""
        self._running = False
        self.client.disconnect()

        print(f"\n[agent] 退出 — rx={self._stats['rx_bytes']}B asr={self._stats['asr_count']} "
              f"llm={self._stats['llm_count']} tts={self._stats['tts_bytes']}B "
              f"transfers={self._stats['transfers']} hangups={self._stats['hangups']}")

    def stop(self) -> None:
        """外部调用来优雅退出。"""
        self._running = False

    @property
    def stats(self) -> dict:
        return dict(self._stats)
