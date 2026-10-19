"""CIPHER — Leon Integration Configuration.

Enforces local-first constraints, loopback network binding, rejection of cloud models,
version pinning (Leon 1.x stable vs unstable 2.0 developer preview), and tool/skill
isolation verification.

Notice on Tool/Skill Boundaries:
Upstream Leon's /api/v1/query endpoint dispatches utterances to skill actions ('run-action')
on the host operating system. Because Leon does not provide a proven text-only or tool-isolated
execution mode, tool/skill isolation cannot be proven. Under CIPHER safety policy, live
execution is disabled and unavailable by default (enable_live_execution=False).
"""

from dataclasses import dataclass, field
import os
from pathlib import Path
from typing import Optional, Tuple

from integrations.contract import validate_loopback_url


@dataclass
class LeonConfig:
    """Configuration settings for the optional Leon integration."""

    # Integration enablement (strictly disabled by default)
    enabled: bool = field(
        default_factory=lambda: os.environ.get("CIPHER_LEON_ENABLED", "0").lower() in ("1", "true", "yes")
    )

    # Leon server URL (strictly loopback by default)
    api_url: str = "http://127.0.0.1:1337"

    # Pinned target version (Leon 1.x stable contract)
    pinned_version: str = "1.0.0"

    # Verified stable endpoints for targeted release
    query_endpoint: str = "/api/v1/query"
    info_endpoint: str = "/api/v1/info"

    # Provenance verification: False until verified local runtime confirmed
    verified_local: bool = False

    # Live execution enablement: False by default because Leon /api/v1/query
    # dispatches to host skill actions and cannot prove tool-free text isolation.
    enable_live_execution: bool = False

    # Verified downstream tool isolation: False until an isolated, no-tools local deployment is proven.
    # A Boolean opt-in alone must not be treated as proof that the endpoint is safe.
    verified_tool_isolation: bool = False


    # Profile API Key / Auth token (loaded from env, never hardcoded)
    api_key: Optional[str] = field(
        default_factory=lambda: os.environ.get("CIPHER_LEON_API_KEY")
    )

    # Default model identifier
    default_model: str = "local/default"

    # Per-request timeout in seconds
    timeout_seconds: float = 30.0

    # Local-first enforcement
    strict_local: bool = True

    # Maximum response body bytes to read from stream (1MB default)
    max_output_bytes: int = 1048576

    # Maximum error body bytes to read from HTTP error stream (4KB default)
    max_error_bytes: int = 4096

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

    def is_tool_policy_safe(self) -> Tuple[bool, Optional[str]]:
        """Verifies whether Leon execution can guarantee tool/skill isolation.

        Leon's /api/v1/query route dispatches utterances directly to skill actions
        (run-action) on the host operating system. Because Leon provides no mechanism
        for a pure text-only or isolated execution mode, tool/skill isolation CANNOT
        be proven by a mere boolean opt-in. A Boolean opt-in must not be treated as proof
        that the endpoint is safe. Under CIPHER safety policy, live execution is disabled fail-closed.
        """
        if not self.enable_live_execution or not self.verified_tool_isolation:
            return False, (
                "Leon tool/skill isolation cannot be proven: /api/v1/query dispatches "
                "to arbitrary host skill actions ('run-action'). Safe text-only mode cannot be established. "
                "A Boolean opt-in does not establish endpoint safety. "
                "Live execution is disabled under CIPHER safety policy."
            )
        return True, None


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
        """Ensures server communication is strictly bound to loopback using shared validator."""
        return validate_loopback_url(
            self.api_url,
            allowed_schemes=("http", "https"),
            description="Leon API URL"
        )
