"""CIPHER — Dodaios Integration Configuration.

Enforces local-first constraints, loopback network binding, rejection of cloud models,
and least-privileged access (strictly blocking computer use and mission dispatch).
"""

from dataclasses import dataclass, field
import os
from pathlib import Path
from typing import Optional, Tuple


@dataclass
class DodaiosConfig:
    """Configuration settings for the optional Dodaios integration."""

    # Integration enablement (strictly disabled by default)
    enabled: bool = field(
        default_factory=lambda: os.environ.get("CIPHER_DODAIOS_ENABLED", "0").lower() in ("1", "true", "yes")
    )

    # Control REST API URL (strictly loopback by default)
    api_url: str = "http://127.0.0.1:8000"

    # Optional bearer token / API key (loaded from env, never hardcoded)
    api_key: Optional[str] = field(
        default_factory=lambda: os.environ.get("CIPHER_DODAIOS_API_KEY")
    )

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
        """Ensures Control API communication is strictly bound to loopback using parsed hostname."""
        try:
            import urllib.parse
            parsed = urllib.parse.urlsplit(self.api_url)
            hostname = (parsed.hostname or "").lower()
            if hostname not in ("127.0.0.1", "localhost", "::1"):
                return False, f"Dodaios API URL '{self.api_url}' host '{hostname}' is not bound to a loopback address (127.0.0.1, localhost, ::1)."
            return True, None
        except Exception as e:
            return False, f"Invalid Dodaios API URL: {e}"
