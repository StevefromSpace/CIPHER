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

    # Explicit tool denial verification: 'minimal' alone still includes presence, session_status, gateway.
    # explicit_tool_denial must be True to guarantee fail-closed tool isolation without modifying global config.
    explicit_tool_denial: bool = True

    # Arguments passed to enforce explicit tool denial on CLI without modifying global configuration
    tool_denial_args: Tuple[str, ...] = ("--deny-tools", "*")

    # Flag indicating whether local execution has been independently verified (default False -> UNKNOWN route)
    verified_local: bool = False

    # Permitted local provider prefixes (advisory only; model prefix is not proof of local inference)
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
        """Validates model identifier against known cloud patterns.
        
        Notice: Having a local prefix like 'ollama/' is an advisory sanity check,
        not cryptographic proof of local inference. Route remains UNKNOWN unless verified.
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
        """Ensures gateway communication is strictly bound to loopback using shared URL validator."""
        from integrations.contract import validate_loopback_url
        return validate_loopback_url(
            self.gateway_url,
            allowed_schemes=("ws", "wss"),
            description="Gateway URL"
        )

    def is_tool_policy_safe(self) -> Tuple[bool, Optional[str]]:
        """Verifies that the active OpenClaw profile verifiably restricts tool execution.

        A profile named 'minimal' is NOT proof that tools are disabled because upstream
        minimal profile still includes presence, session_status, and gateway discovery tools.
        Explicit tool denial (explicit_tool_denial=True) must be active.
        """
        if not self.enforce_minimal_tool_profile:
            return False, "Tool policy enforcement cannot be disabled. CIPHER requires tools to be locked down."

        clean_profile = self.tool_profile.strip().lower()
        if clean_profile != "minimal":
            return False, (
                f"Active OpenClaw tool profile is '{self.tool_profile}'. "
                "Only the isolated 'minimal' profile with explicit tool denial is permitted by CIPHER policy."
            )

        if not self.explicit_tool_denial:
            return False, (
                "OpenClaw profile 'minimal' alone does not guarantee tool lockdown (it permits internal "
                "discovery and gateway tools). Explicit tool denial (explicit_tool_denial=True) is required."
            )

        return True, None
