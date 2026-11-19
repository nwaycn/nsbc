"""Whisper OpenAI ASR Provider — POST audio/transcriptions → 文字

API 文档：https://platform.openai.com/docs/api-reference/audio/createTranscription
零第三方依赖，只用 urllib（stdlib）。multipart/form-data 手动构造。

输入：PCMU 8kHz 原始字节（nsbc 媒体管道就是这个格式）
处理：PCMU → PCM16 → 写 temp .wav（44 字节 header）→ multipart POST → parse .text

为什么不直接传 PCMU：OpenAI Whisper API 偏好 WAV / FLAC / MP3，原始 PCMU 不接受。
这里生成最小可接受的 44 字节 RIFF header + PCM16 data，兼容 OpenAI / 阿里云 / 腾讯云 ASR。"""
from __future__ import annotations

import io
import json
import os
import ssl
import tempfile
import urllib.request
import urllib.error
from typing import Optional

from .base import ASRProvider


def _ssl_ctx() -> ssl.SSLContext:
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


class WhisperASR(ASRProvider):
    """Whisper ASR — 支持 OpenAI 和任何兼容 audio/transcriptions 的服务"""

    DEFAULT_ENDPOINT = "https://api.openai.com/v1/audio/transcriptions"
    DEFAULT_MODEL = "whisper-1"

    def __init__(self,
                 api_key: str,
                 model: str = DEFAULT_MODEL,
                 endpoint: str = DEFAULT_ENDPOINT,
                 language: str = "",
                 timeout_s: int = 30):
        if not api_key:
            raise ValueError("Whisper api_key 不能为空")
        self.api_key = api_key
        self.model = model
        self.endpoint = endpoint
        self.language = language  # 空串 = 自动检测
        self.timeout_s = timeout_s

    def transcribe(self, pcmu_bytes: bytes) -> str:
        if not pcmu_bytes:
            return ""

        # 1. PCMU → PCM16 → WAV temp file
        try:
            wav_bytes = _pcmu_to_wav_pcm16(pcmu_bytes, sample_rate=8000)
        except Exception as e:
            print(f"[WhisperASR] PCMU→WAV 失败: {e}", flush=True)
            return ""

        # 2. multipart POST
        boundary = "----NSBCWhisperBoundary7MA4YWxkTrZu0gW"
        parts = []

        # model part
        model_body = json.dumps({"model": self.model})
        parts.append(self._form_part(boundary, "model", model_body))

        # language part（可选）
        if self.language:
            parts.append(self._form_part(boundary, "language", self.language))

        # file part
        file_header = (
            f'--{boundary}\r\n'
            f'Content-Disposition: form-data; name="file"; filename="audio.wav"\r\n'
            f'Content-Type: audio/wav\r\n\r\n'
        ).encode("utf-8")
        parts.append(file_header + wav_bytes + b"\r\n")

        # final boundary
        parts.append(f'--{boundary}--\r\n'.encode("utf-8"))

        body = b"".join(parts)

        req = urllib.request.Request(self.endpoint, data=body, method="POST")
        req.add_header("Content-Type", f"multipart/form-data; boundary={boundary}")
        req.add_header("Authorization", f"Bearer {self.api_key}")

        try:
            with urllib.request.urlopen(req, timeout=self.timeout_s, context=_ssl_ctx()) as resp:
                data = resp.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            detail = e.read().decode("utf-8", "replace")[:300] if hasattr(e, "read") else str(e)
            print(f"[WhisperASR] HTTP {e.code}: {detail}", flush=True)
            return ""
        except Exception as e:
            print(f"[WhisperASR] 请求失败: {e}", flush=True)
            return ""

        # 3. parse {"text": "..."}
        try:
            blob = json.loads(data)
            text = blob.get("text", "")
            if isinstance(text, str):
                return text.strip()
        except (json.JSONDecodeError, TypeError):
            pass

        # fallback: 正则找 "text":"..."
        marker = '"text"'
        idx = data.find(marker)
        if idx >= 0:
            q_start = data.find('"', idx + len(marker))
            if q_start >= 0:
                q_end = data.find('"', q_start + 1)
                if q_end >= 0:
                    return data[q_start + 1:q_end]

        return ""

    @staticmethod
    def _form_part(boundary: str, name: str, value: str) -> bytes:
        header = (
            f'--{boundary}\r\n'
            f'Content-Disposition: form-data; name="{name}"\r\n\r\n'
        ).encode("utf-8")
        return header + value.encode("utf-8") + b"\r\n"


def _pcmu_to_wav_pcm16(pcmu_bytes: bytes, sample_rate: int = 8000) -> bytes:
    """PCMU (μ-law) 8kHz → WAV (PCM16 LE) 8kHz。返回完整 RIFF bytes。"""
    import struct

    n_samples = len(pcmu_bytes)
    pcm16 = bytearray(n_samples * 2)

    for i, b in enumerate(pcmu_bytes):
        sample = _pcmu_to_pcm16(b)
        struct.pack_into("<h", pcm16, i * 2, sample)

    # RIFF header: 44 bytes
    data_size = len(pcm16)
    riff = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        data_size + 36,   # riff_len = filesize - 8
        b"WAVE",
        b"fmt ",
        16,               # fmt chunk size
        1,                # audio format (PCM)
        1,                # channels
        sample_rate,
        sample_rate * 2,  # byte rate
        2,                # block align
        16,               # bits per sample
        b"data",
        data_size,
    )
    return riff + bytes(pcm16)


def _pcmu_to_pcm16(ulaw: int) -> int:
    """G.711 μ-law 8-bit → PCM16 signed int。μ-law 解码逆变换。"""
    ulaw = ~ulaw & 0xFF
    sign = (ulaw & 0x80) >> 7
    exponent = (ulaw >> 4) & 0x07
    mantissa = ulaw & 0x0F

    # 16-bit 解码：reference level 2147483648
    sample = (mantissa << 3) | 0x84
    if exponent > 0:
        sample = sample << exponent

    sample = sample - 0x84
    if sign:
        sample = -sample

    # 缩放到 int16 范围（G.711 μ-law 解码后是 int14，左移 2 位填满 int16）
    sample = sample << 2
    if sample > 32767:
        sample = 32767
    if sample < -32768:
        sample = -32768

    return sample
