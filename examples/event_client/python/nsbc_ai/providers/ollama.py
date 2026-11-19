"""Ollama LLM Provider — 本地/自建 LLM /api/chat → LLMReply

API 文档：https://github.com/ollama/ollama/blob/main/docs/api.md#generate-a-chat-completion
零第三方依赖，只用 urllib（stdlib）。支持任何兼容 chat completion 的端点。

设计（来自 experience 1280962 Provider 抽象经验）：
  - Ollama / OpenAI Chat / DeepSeek Chat 结构类似，endpoint / model_id / api_key 参数化
  - 统一要求 LLM 输出 **结构化 JSON**（JSON mode），parse 后转 LLMReply
  - Agent 主循环只看 LLMReply.action，不关心后端是哪个 LLM

JSON mode prompt（system_prompt 注入）：
  "你是一个电话客服 AI。所有回复必须是严格 JSON，不包含任何其他文字：
   {\"action\":\"say|transfer|hangup|silence\", \"text\":\"...\", \"dest\":\"...\", \"reason\":\"...\"}
   transfer 时 dest 必须填写目标分机号（如 1001）。"
"""
from __future__ import annotations

import json
import ssl
import urllib.request
import urllib.error
from typing import Optional

from .base import LLMProvider, LLMReply


def _ssl_ctx() -> ssl.SSLContext:
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    return ctx


# system prompt: 强制 JSON mode + function call schema
_DEFAULT_SYSTEM_PROMPT = (
    "你是一个电话客服 AI。所有回复必须是严格 JSON，不包含任何其他文字。"
    "JSON schema：{\"action\":\"say|transfer|hangup|silence\", \"text\":\"...\", \"dest\":\"...\", \"reason\":\"...\"}"
    "action=transfer 时 dest 必须填写目标分机号（如 1001）。"
    "action=say 时 text 必须是自然语言回复。action=silence 时不要 text。"
    "如果用户说'转人工'、'人工'、'找客服'，action=transfer。"
    "如果用户说'挂断'、'再见'、'bye'，action=hangup。"
)


class OllamaLLM(LLMProvider):
    """Ollama /api/chat LLM — 本地或自建（OpenAI Chat API 兼容的也能用，改 endpoint 即可）"""

    DEFAULT_ENDPOINT = "http://localhost:11434/api/chat"
    DEFAULT_MODEL = "llama3.1:8b"

    def __init__(self,
                 model: str = DEFAULT_MODEL,
                 endpoint: str = DEFAULT_ENDPOINT,
                 api_key: str = "",
                 system_prompt: str = _DEFAULT_SYSTEM_PROMPT,
                 transfer_keywords: Optional[list[str]] = None,
                 transfer_dest: str = "1001",
                 timeout_s: int = 30):
        self.model = model
        self.endpoint = endpoint
        self.api_key = api_key
        self.system_prompt = system_prompt
        self.transfer_keywords = [k.lower() for k in (transfer_keywords or ["转人工", "人工", "agent", "human"])]
        self.transfer_dest = transfer_dest
        self.timeout_s = timeout_s

    def chat(self, transcript: str) -> LLMReply:
        if not transcript:
            return LLMReply.silence()

        body = json.dumps({
            "model": self.model,
            "stream": False,
            "messages": [
                {"role": "system", "content": self.system_prompt},
                {"role": "user", "content": transcript},
            ],
            "format": "json",  # Ollama 原生 JSON mode；不支持的端点会忽略
        }).encode("utf-8")

        req = urllib.request.Request(self.endpoint, data=body, method="POST")
        req.add_header("Content-Type", "application/json")
        if self.api_key:
            req.add_header("Authorization", f"Bearer {self.api_key}")

        try:
            with urllib.request.urlopen(req, timeout=self.timeout_s, context=_ssl_ctx()) as resp:
                data = resp.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            detail = e.read().decode("utf-8", "replace")[:300] if hasattr(e, "read") else str(e)
            print(f"[OllamaLLM] HTTP {e.code}: {detail}", flush=True)
            return self._keyword_fallback(transcript)
        except Exception as e:
            print(f"[OllamaLLM] 请求失败: {e}", flush=True)
            return self._keyword_fallback(transcript)

        # parse response.message.content
        try:
            blob = json.loads(data)
            content = blob.get("message", {}).get("content", "")
            return self._parse_llm_content(content, transcript)
        except (json.JSONDecodeError, TypeError):
            print(f"[OllamaLLM] JSON parse 失败: {data[:200]}", flush=True)
            return self._keyword_fallback(transcript)

    # ---------------------------------------------------------------------
    # 内部：解析 LLM 返回内容 → LLMReply；解析失败走 keyword fallback
    # ---------------------------------------------------------------------

    def _parse_llm_content(self, content: str, transcript: str) -> LLMReply:
        """先尝试严格 JSON parse → 失败走 keyword fallback。"""
        if not content:
            return self._keyword_fallback(transcript)

        # 可能被 markdown ```json ``` 包裹
        content = content.strip()
        if content.startswith("```"):
            lines = content.split("\n")
            lines = [l for l in lines if not l.startswith("```")]
            content = "\n".join(lines).strip()

        try:
            blob = json.loads(content)
        except json.JSONDecodeError:
            return self._keyword_fallback(transcript)

        action = str(blob.get("action", "silence")).lower()
        text = blob.get("text", "")
        dest = blob.get("dest", "") or self.transfer_dest
        reason = blob.get("reason", "")

        if action == "transfer":
            return LLMReply.transfer(dest=dest, reason=reason)
        if action == "hangup":
            return LLMReply.hangup(reason=reason)
        if action == "silence":
            return LLMReply.silence(reason=reason)
        if action == "say" and text:
            return LLMReply.say(text=text, reason=reason)

        # action 未知 / text 为空 → fallback
        return self._keyword_fallback(transcript)

    def _keyword_fallback(self, transcript: str) -> LLMReply:
        """LLM 失败或输出异常时的纯关键词 fallback —— 保证 AI 不会哑掉。"""
        lowered = transcript.lower()
        for kw in self.transfer_keywords:
            if kw in lowered:
                return LLMReply.transfer(dest=self.transfer_dest, reason=f"fallback keyword={kw}")

        # 默认 say —— 至少告诉用户"我没听清"
        return LLMReply.say(text=f"我收到了您说的：{transcript[:40]}。请问还需要什么帮助吗？",
                            reason="fallback (LLM unavailable)")


# =============================================================================
# 便捷工厂：从 env 或 kwargs 构建 LLMProvider（mock / ollama / openai_chat）
# =============================================================================

def create_llm(provider: str = "ollama", **kwargs) -> LLMProvider:
    """工厂函数。provider: 'ollama' | 'mock'。"""
    if provider == "mock":
        from .mock import MockLLM
        return MockLLM(**kwargs)

    if provider in ("ollama", "openai_chat", "deepseek"):
        # OpenAI Chat / DeepSeek Chat endpoint 和 Ollama 不同，kwargs 传 endpoint 即可
        return OllamaLLM(**kwargs)

    raise ValueError(f"Unknown LLM provider: {provider}")
