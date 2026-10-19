"""CIPHER — Assistant Integration Shared Contract.

Defines the abstract interface, data structures, and central policy validation
for replaceable assistant backends in CIPHER. Mirrors the C++17 cipher_assistant library.
Neither cipher_core nor cipher_memory depends on this module.
"""

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from enum import Enum
import math
import re
from typing import Any, Dict, Optional, Tuple
import urllib.error
import urllib.request


class ProcessingRoute(str, Enum):
    """Provenance and network routing of the model execution."""
    UNKNOWN = "unknown"
    VERIFIED_LOCAL = "verified_local"
    REMOTE_CLOUD = "remote_cloud"
    MIXED = "mixed"


class AssistantStatus(str, Enum):
    """Execution status returned by an assistant backend."""
    OK = "ok"
    DISABLED = "disabled"
    UNAVAILABLE = "unavailable"
    CONNECTION_ERROR = "connection_error"
    AUTH_ERROR = "auth_error"
    TIMEOUT = "timeout"
    ERROR = "error"
    REJECTED_CLOUD_MODEL = "rejected_cloud_model"
    REJECTED_TOOLS_ENABLED = "rejected_tools_enabled"
    INVALID_REQUEST = "invalid_request"


@dataclass
class AssistantCapabilities:
    """Declared capabilities and constraints of an assistant backend."""
    name: str
    version: str = "unknown"
    supports_text: bool = True
    supports_streaming: bool = False
    supports_sessions: bool = False
    supports_voice_events: bool = False
    supports_tools: bool = False
    default_route: ProcessingRoute = ProcessingRoute.UNKNOWN
    is_mock_only: bool = False


@dataclass
class AssistantRequest:
    """Standardized prompt submission to an assistant backend."""
    prompt: str
    session_id: Optional[str] = None
    timeout_seconds: float = 30.0
    allow_tools: bool = False
    require_local: bool = True
    model_override: Optional[str] = None
    metadata: Dict[str, Any] = field(default_factory=dict)


@dataclass
class AssistantResponse:
    """Standardized response from an assistant backend."""
    status: AssistantStatus
    text: str = ""
    model: str = ""
    route: ProcessingRoute = ProcessingRoute.UNKNOWN
    duration_ms: float = 0.0
    error: Optional[str] = None
    raw_output: Optional[str] = None

    @property
    def is_ok(self) -> bool:
        return self.status == AssistantStatus.OK


def sanitize_sensitive_data(message: str) -> str:
    """Strips credentials, tokens, API keys, or private authorization headers from logs/errors."""
    if not message:
        return ""
    sanitized = re.sub(r'(bearer\s+)[a-zA-Z0-9_\-\.]{8,}', r'\1[REDACTED]', message, flags=re.IGNORECASE)
    sanitized = re.sub(r'(token\s*[:=]\s*)[a-zA-Z0-9_\-\.]{8,}', r'\1[REDACTED]', sanitized, flags=re.IGNORECASE)
    sanitized = re.sub(r'(key\s*[:=]\s*)[a-zA-Z0-9_\-\.]{8,}', r'\1[REDACTED]', sanitized, flags=re.IGNORECASE)
    sanitized = re.sub(r'(password\s*[:=]\s*)[^\s,;&]+', r'\1[REDACTED]', sanitized, flags=re.IGNORECASE)
    sanitized = re.sub(r'(x-api-key\s*[:=]\s*)[a-zA-Z0-9_\-\.]{8,}', r'\1[REDACTED]', sanitized, flags=re.IGNORECASE)
    return sanitized.strip()


class NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    """Rejects automatic HTTP redirects fail-closed to prevent SSRF and egress escapes."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise urllib.error.HTTPError(
            newurl,
            code,
            f"Automatic HTTP redirect to '{newurl}' is blocked for security.",
            headers,
            fp
        )



def validate_assistant_request(
    request: AssistantRequest,
    capabilities: AssistantCapabilities
) -> Tuple[bool, Optional[AssistantResponse]]:
    """Central policy validation gate for all CIPHER assistant requests.
    
    Invariants enforced:
    1. Blank or whitespace-only prompts rejected.
    2. Timeouts bounded between 1.0s and 300.0s.
    3. Tool execution fail-closed: rejected if requested or if backend has tools active.
    4. Local-only policy: reject if require_local=True but route is not VERIFIED_LOCAL.
    """
    # Invariant 1: Blank prompts
    if not request.prompt or not request.prompt.strip():
        return False, AssistantResponse(
            status=AssistantStatus.INVALID_REQUEST,
            error="Prompt cannot be empty or whitespace only.",
            route=capabilities.default_route
        )

    # Invariant 2: Timeout bounds & finiteness
    if (
        not isinstance(request.timeout_seconds, (int, float))
        or not math.isfinite(request.timeout_seconds)
        or request.timeout_seconds < 1.0
        or request.timeout_seconds > 300.0
    ):
        return False, AssistantResponse(
            status=AssistantStatus.INVALID_REQUEST,
            error=f"Timeout seconds ({request.timeout_seconds}) must be a finite number bounded between 1.0 and 300.0 seconds.",
            route=capabilities.default_route
        )

    # Invariant 3: Tool lockdown
    if request.allow_tools:
        return False, AssistantResponse(
            status=AssistantStatus.REJECTED_TOOLS_ENABLED,
            error="Tool execution is currently locked down in CIPHER. Requests with allow_tools=True are rejected.",
            route=capabilities.default_route
        )

    # Invariant 4: Local route verification
    if request.require_local and capabilities.default_route != ProcessingRoute.VERIFIED_LOCAL:
        return False, AssistantResponse(
            status=AssistantStatus.REJECTED_CLOUD_MODEL,
            error=f"Local-only policy requires verified local route, but backend '{capabilities.name}' route is '{capabilities.default_route.value}'.",
            route=capabilities.default_route
        )

    return True, None


class BaseAssistantBackend(ABC):
    """Abstract base class for optional CIPHER assistant capability providers."""

    @property
    @abstractmethod
    def capabilities(self) -> AssistantCapabilities:
        """Return backend metadata and capability declarations."""
        pass

    @abstractmethod
    def is_available(self) -> bool:
        """Check whether the assistant backend runtime is reachable and ready."""
        pass

    @abstractmethod
    def get_status(self) -> Dict[str, Any]:
        """Return diagnostic health and configuration state (strictly sanitized, no secrets)."""
        pass

    @abstractmethod
    def submit_prompt(self, request: AssistantRequest) -> AssistantResponse:
        """Submit a single text prompt and receive the resulting text response.
        
        Must pass through validate_assistant_request() before external execution.
        Must handle errors gracefully without throwing unhandled exceptions.
        """
        pass
