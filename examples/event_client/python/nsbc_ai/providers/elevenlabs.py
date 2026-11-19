"""ElevenLabs TTS Provider — text-to-speech REST → PCMU 8kHz

API 文档：https://elevenlabs.io/docs/api-reference/text-to-speech
零第三方依赖，只用 urllib（stdlib）。

请求流程：
  POST https://api.elevenlabs.io/v1/text-to-speech/{voice_id}?output_format=pcm_16000
  Header: xi-api-key
  Body: {"text": "...", "model_id": "eleven_monolingual_v1"}

响应：PCM 16kHz raw 二进制 → float → PCM16 → PCMU 线性量化

为什么内部要重采样：nsbc 媒体管道跑 8kHz PCMU，ElevenLabs 推荐输出 16kHz PCM。
这里实现一个轻量 16kHz→8kHz 降采样 + PCM16→PCMU 变换。"""
from __future__ import annotations

import io
import json
import ssl
import urllib.request
import urllib.error
from typing import Optional

from .base import TTSProvider


def _ssl_ctx() -> ssl.SSLContext:
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


class ElevenLabsTTS(TTSProvider):
    """ElevenLabs REST TTS → PCMU 8kHz"""

    DEFAULT_ENDPOINT = "https://api.elevenlabs.io/v1/text-to-speech"
    DEFAULT_MODEL = "eleven_monolingual_v1"
    DEFAULT_VOICE_ID = "21m00Tcm4TlvDq8ikWAM"  # Rachel demo voice

    def __init__(self,
                 api_key: str,
                 voice_id: str = DEFAULT_VOICE_ID,
                 model_id: str = DEFAULT_MODEL,
                 endpoint: str = DEFAULT_ENDPOINT,
                 timeout_s: int = 15):
        if not api_key:
            raise ValueError("ElevenLabs api_key 不能为空")
        self.api_key = api_key
        self.voice_id = voice_id
        self.model_id = model_id
        self.endpoint = endpoint
        self.timeout_s = timeout_s

    def synthesize(self, text: str) -> bytes:
        if not text:
            return b""

        url = f"{self.endpoint}/{self.voice_id}?output_format=pcm_16000"
        body = json.dumps({
            "text": text,
            "model_id": self.model_id,
        }).encode("utf-8")

        req = urllib.request.Request(url, data=body, method="POST")
        req.add_header("Content-Type", "application/json")
        req.add_header("xi-api-key", self.api_key)
        req.add_header("Accept", "audio/pcm")

        try:
            with urllib.request.urlopen(req, timeout=self.timeout_s, context=_ssl_ctx()) as resp:
                pcm16_raw = resp.read()
        except urllib.error.HTTPError as e:
            detail = e.read().decode("utf-8", "replace")[:200] if hasattr(e, "read") else str(e)
            print(f"[ElevenLabsTTS] HTTP {e.code}: {detail}", flush=True)
            return b""
        except Exception as e:
            print(f"[ElevenLabsTTS] 请求失败: {e}", flush=True)
            return b""

        if not pcm16_raw or len(pcm16_raw) < 2:
            return b""

        return self._pcm16_16k_to_pcmu_8k(pcm16_raw)

    # =====================================================================
    # 轻量重采样 + PCM16 → PCMU（无第三方依赖）
    # =====================================================================

    @staticmethod
    def _pcm16_16k_to_pcmu_8k(pcm16_raw: bytes) -> bytes:
        """16kHz PCM16 LE → 8kHz PCMU（每 2 个样本取 1 个 + 线性量化）。
        PCMU 量化参考：https://en.wikipedia.org/wiki/%C2%B5-law_algorithm"""
        import struct

        n_samples = len(pcm16_raw) // 2
        out = bytearray(n_samples // 2)

        for i in range(n_samples // 2):
            # 取偶数下标的样本（简单降采样 2x）
            raw = struct.unpack_from("<h", pcm16_raw, i * 4)[0]
            out[i] = _pcm16_to_pcmu(raw)

        return bytes(out)


def _pcm16_to_pcmu(sample: int) -> int:
    """PCM16 signed int16 → PCMU 8-bit unsigned。算法：G.711 μ-law。
    参考 FreeSWITCH mod_slin2ulaw 的实现。"""
    # sign bit
    sign = (sample >> 8) & 0x80
    if sign:
        sample = -sample

    # clip
    if sample > 32635:
        sample = 32635

    # 线性量化 14-bit
    sample += 0x84
    exponent = 7
    for i in range(7):
        if (sample & (0x4000 >> i)) != 0:
            exponent = 6 - i
            break

    mantissa = (sample >> (exponent + 3)) & 0x0F
    ulaw = ~(sign | (exponent << 4) | mantissa) & 0xFF
    return ulaw
