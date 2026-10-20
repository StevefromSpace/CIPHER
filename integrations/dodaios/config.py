"""CIPHER — Dodaios Integration Configuration.

Enforces local-first constraints, loopback network binding, rejection of cloud models,
and least-privileged access (strictly blocking computer use and mission dispatch).

Notice: The Dodaios runtime gateway (/api/runtime-gateway/v1/chat/completions) is designed
for agent-runtime model calls and can proxy to tools or hosted providers upstream.
It is NOT a verified no-tools endpoint. Live compatibility is classified as unverified
until an isolated, verified local runtime is validated with explicit proof.
"""

from dataclasses import dataclass, field
import os
from pathlib import Path
from typing import Optional, Tuple

from integrations.contract import validate_loopback_url


@dataclass
class DodaiosConfig:
    """Configuration settings for the optional Dodaios integration."""

    # Integration enablement (strictly disabled by default)
    enabled: bool = field(
        default_factory=lambda: os.environ.get("CIPHER_DODAIOS_ENABLED", "0").lower() in ("1", "true", "yes")
    )

    # Control REST API URL (strictly loopback by default)
    api_url: str = "http://127.0.0.1:8000"

    # Targeted runtime gateway endpoints
    # NOTICE: The runtime gateway is an agent runtime endpoint, NOT a verified tool-free endpoint.
    chat_endpoint: str = "/api/runtime-gateway/v1/chat/completions"
    health_endpoint: str = "/api/health"

    # Provenance verification: operator configuration assertion (default False -> UNKNOWN route).
    # NOTE: verified_local asserts loopback URL compliance per contract, NOT independent hardware or cryptographic proof.
    verified_local: bool = False

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

    # Maximum response body bytes to read from stream (1MB default)
    max_output_bytes: int = 1048576

    # Maximum error body bytes to read from HTTP error stream (4KB default)
    max_error_bytes: int = 4096

    # Live execution enablement: strictly False by default.
    # The runtime gateway (/api/runtime-gateway/v1/chat/completions) is designed for agent
    # runtimes and can proxy calls to downstream tools and external providers.
    # It is NOT a verified no-tools endpoint.
    enable_live_execution: bool = False

    # Verified downstream tool isolation: False until an isolated, no-tools local deployment is proven.
    verified_tool_isolation: bool = False

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

    def is_tool_policy_safe(self) -> Tuple[bool, Optional[str]]:
        """Verifies whether Dodaios runtime gateway can guarantee tool-free text isolation.

        The runtime gateway (/api/runtime-gateway/v1/chat/completions) is designed for
        agent runtimes and can proxy calls to downstream tools and external providers.
        It is NOT a verified no-tools endpoint. Unless downstream isolation is positively
        established, live execution is disabled fail-closed under CIPHER safety policy.
        """
        if not self.enable_live_execution or not self.verified_tool_isolation:
            return False, (
                "Dodaios runtime gateway is an agent-runtime proxy, not a verified no-tools endpoint. "
                "Downstream tool isolation cannot be proven. Live execution is disabled fail-closed under CIPHER safety policy."
            )
        return True, None

    def validate_api_url(self) -> Tuple[bool, Optional[str]]:
        """Ensures Control API communication is strictly bound to loopback using shared validator."""
        return validate_loopback_url(
            self.api_url,
            allowed_schemes=("http", "https"),
            description="Dodaios API URL"
        )
