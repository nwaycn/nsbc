"""BargeInController — VAD-based 用户打断控制器

逻辑（和 Go agent / Rust ws.rs 对齐）：
  1. AI 正在说话（tx_audio 循环中）时，并行 vad_state 轮询 A-leg（用户侧）
  2. VAD_STATE_SPEECH 持续出现 → 用户在打断 → 立即停 ai_send_tx
  3. AI 说话前查一次 VAD → 用户在说就 defer（等用户说完）

nsbc 侧 ai-set-producer 自动启动 VAD，本模块只管查状态 + 等静默。"""
from __future__ import annotations

import time
from typing import Optional

from .client import NsbcMediaClient


class BargeInController:
    """REQ-AI-BARGEIN-001: VAD-based barge-in 控制器。"""

    def __init__(self, client: NsbcMediaClient, rms_threshold: int = 800):
        self.client = client
        self.rms_threshold = rms_threshold
        self.barge_in_triggered = False
        self.barge_in_count = 0

    def user_speaking(self) -> bool:
        """查 VAD A-leg（用户侧）是否在说话。"""
        state = self.client.vad_state()
        return state.a_state == "SPEECH" and state.a_rms >= self.rms_threshold

    def wait_until_silence(self, timeout_s: float = 2.0, poll_interval_s: float = 0.05) -> bool:
        """等用户说完（连续 N 次 SILENCE）。超时返回 False。"""
        consecutive_silence = 0
        required = 10   # 10 × 50ms = 500ms 连续静默才认为说完
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            if self.user_speaking():
                consecutive_silence = 0
            else:
                consecutive_silence += 1
                if consecutive_silence >= required:
                    return True
            time.sleep(poll_interval_s)
        return False
