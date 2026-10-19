"""CIPHER — Leon Integration Configuration.

Enforces local-first constraints, loopback network binding, rejection of cloud models,
and version pinning (Leon 1.x stable vs unstable 2.0 developer preview).
"""

from dataclasses import dataclass, field
import os
from pathlib import Path
from typing import Optional, Tuple


@dataclass
class LeonConfig:
    """Configuration settings for the optional Leon integration."""

    # Integration enablement (strictly disabled by default)
    enabled: bool = field(
        default_factory=lambda: os.environ.get("CIPHER_LEON_ENABLED", "0").lower() in ("1", "true", "yes")
    )

    # Leon server URL (strictly loopback by default)
    api_url: str = "http://127.0.0.1:1337"

    # Profile API Key / Auth token (loaded from env, never hardcoded)
    api_key: Optional[str] = field(
        default_factory=lambda: os.environ.get("CIPHER_LEON_API_KEY")
    )

    # Pinned target version (Leon 1.x stable contract)
    pinned_version: str = "1.0.0"

    # Default model identifier
    default_model: str = "local/default"

    # Per-request timeout in seconds
    timeout_seconds: float = 30.0

    # Local-first enforcement
    strict_local: bool = True

    # Permitted local provider prefixes
    allowed_local_prefixes: Tuple[str, ...] = (
        "local/",
        "ollama/",
        "lmstudio/",
        "llamacpp/",
        "vllm/",
    )

    # Forbidden cloud patterns
    forbidden_cloud_patterns: Tuple[str, ...] = (
        "openai",
        "anthropic",
        "claude",
        "gpt-",
        "gpt_",
        "ollama-cloud",
        "gemini",
        "mistral-ai",
        "bedrock",
        "azure",
        "groq",
        "together",
        "fireworks",
        "deepseek-cloud",
    )

    def is_model_local(self, model_name: str) -> Tuple[bool, Optional[str]]:
        """Validates whether a model specification is strictly local-first."""
        if not model_name or not model_name.strip():
            return False, "Model identifier is empty."

        clean_name = model_name.strip().lower()

        for pattern in self.forbidden_cloud_patterns:
            if pattern in clean_name:
                return False, f"Model '{model_name}' matches cloud pattern '{pattern}'."

        if not self.strict_local:
            return True, None

        has_local_prefix = any(clean_name.startswith(p) for p in self.allowed_local_prefixes)
        if not has_local_prefix:
            return False, (
                f"Model '{model_name}' lacks an approved local prefix "
                f"({', '.join(self.allowed_local_prefixes)}) under strict local mode."
            )

        return True, None

    def validate_api_url(self) -> Tuple[bool, Optional[str]]:
        """Ensures server communication is strictly bound to loopback."""
        url = self.api_url.lower()
        is_loopback = (
            "127.0.0.1" in url
            or "localhost" in url
            or "[::1]" in url
        )
        if not is_loopback:
            return False, f"Leon API URL '{self.api_url}' is not bound to a loopback address (127.0.0.1/localhost)."
        return True, None
