"""CIPHER — OpenClaw Integration Configuration.

Enforces local-first constraints, loopback network binding, rejection of cloud models,
and strict tool policy restriction (minimal profile without tool execution).
"""

from dataclasses import dataclass, field
import os
from pathlib import Path
import re
from typing import Optional, Set, Tuple


@dataclass
class OpenClawConfig:
    """Configuration settings for the optional OpenClaw integration."""

    # Integration enablement (strictly disabled by default)
    enabled: bool = field(
        default_factory=lambda: os.environ.get("CIPHER_OPENCLAW_ENABLED", "0").lower() in ("1", "true", "yes")
    )

    # Path to openclaw executable (None = search PATH)
    cli_path: Optional[Path] = None

    # Gateway WebSocket URL (strictly loopback by default)
    gateway_url: str = "ws://127.0.0.1:18789"

    # Default model (strictly local provider reference, e.g. Ollama)
    default_model: str = "ollama/llama3.2"

    # Per-request timeout in seconds
    timeout_seconds: float = 30.0

    # Local-first enforcement: reject any cloud routing or non-local models
    strict_local: bool = True

    # Tool policy profile (strictly 'minimal' to disable tool execution)
    tool_profile: str = "minimal"

    # Enforce minimal tool profile to prevent tool execution
    enforce_minimal_tool_profile: bool = True

    # Permitted local provider prefixes
    allowed_local_prefixes: Tuple[str, ...] = (
        "ollama/",
        "local/",
        "lmstudio/",
        "llamacpp/",
        "vllm/",
    )

    # Patterns indicating cloud or hosted model providers
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
        """Validates whether a model specification is strictly local-first.
        
        Returns:
            Tuple of (is_local: bool, rejection_reason: Optional[str])
        """
        if not model_name or not model_name.strip():
            return False, "Model identifier is empty."

        clean_name = model_name.strip().lower()

        # Check for explicit forbidden cloud patterns
        for pattern in self.forbidden_cloud_patterns:
            if pattern in clean_name:
                return False, f"Model '{model_name}' matches cloud pattern '{pattern}'."

        if not self.strict_local:
            return True, None

        # Under strict_local, model must start with an approved local prefix
        has_local_prefix = any(clean_name.startswith(p) for p in self.allowed_local_prefixes)
        if not has_local_prefix:
            return False, (
                f"Model '{model_name}' lacks an approved local prefix "
                f"({', '.join(self.allowed_local_prefixes)}) under strict local mode."
            )

        return True, None

    def validate_gateway_url(self) -> Tuple[bool, Optional[str]]:
        """Ensures gateway communication is strictly bound to loopback using parsed hostname."""
        try:
            import urllib.parse
            parsed = urllib.parse.urlsplit(self.gateway_url)
            hostname = (parsed.hostname or "").lower()
            if hostname not in ("127.0.0.1", "localhost", "::1"):
                return False, f"Gateway URL '{self.gateway_url}' host '{hostname}' is not bound to a loopback address (127.0.0.1, localhost, ::1)."
            return True, None
        except Exception as e:
            return False, f"Invalid Gateway URL: {e}"

    def is_tool_policy_safe(self) -> Tuple[bool, Optional[str]]:
        """Verifies that the active OpenClaw profile restricts tool execution."""
        if not self.enforce_minimal_tool_profile:
            return True, None

        clean_profile = self.tool_profile.strip().lower()
        if clean_profile != "minimal":
            return False, (
                f"Active OpenClaw tool profile is '{self.tool_profile}'. "
                "Only the isolated 'minimal' profile (tools disabled) is permitted by CIPHER policy."
            )
        return True, None
