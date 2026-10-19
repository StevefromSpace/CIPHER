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
    """Strips credentials, tokens, API keys, basic auth, or private authorization headers from logs/errors."""
    if not message:
        return ""
    # Redact URL embedded credentials (user:pass@host)
    sanitized = re.sub(r'://([^:]+):([^@]+)@', r'://\1:[REDACTED]@', message)
    # Redact authorization tokens and keys (with space, colon, or equals separator)
    sanitized = re.sub(r'((?:bearer|token|key|password|x-api-key|api[_\-]?key)\s*[:=\s]\s*)[a-zA-Z0-9_\-\.]{8,}', r'\1[REDACTED]', sanitized, flags=re.IGNORECASE)
    sanitized = re.sub(r'(password\s*[:=\s]\s*)[^\s,;&]+', r'\1[REDACTED]', sanitized, flags=re.IGNORECASE)
    return sanitized.strip()


def validate_loopback_url(
    url: str,
    allowed_schemes: Tuple[str, ...],
    allowed_ports: Optional[Tuple[int, ...]] = None,
    description: str = "URL"
) -> Tuple[bool, Optional[str]]:
    """Strictly validates that a URL is bound to local loopback without user credentials.

    Invariants enforced:
    1. URL must be non-empty string.
    2. Scheme must match allowed_schemes exactly (case-insensitive).
    3. URL must not contain user credentials (user-info).
    4. Hostname must parse exactly to 127.0.0.1, localhost, or ::1.
    5. Port must be valid (1..65535) and match allowed_ports if specified.
    """
    if not url or not isinstance(url, str) or not url.strip():
        return False, f"{description} cannot be empty."

    try:
        import urllib.parse
        parsed = urllib.parse.urlsplit(url.strip())
    except Exception as e:
        return False, f"Invalid {description}: {e}"

    # 1. Scheme validation
    scheme = (parsed.scheme or "").lower()
    allowed_schemes_lower = [s.lower() for s in allowed_schemes]
    if scheme not in allowed_schemes_lower:
        return False, (
            f"{description} '{url}' scheme '{scheme}' is not permitted "
            f"(allowed: {', '.join(allowed_schemes)})."
        )

    # 2. Reject user-info (embedded username/password)
    if parsed.username or parsed.password or "@" in parsed.netloc:
        return False, f"{description} '{url}' must not contain user credentials (user-info)."

    # 3. Exact loopback hostname validation
    hostname = (parsed.hostname or "").lower()
    if hostname not in ("127.0.0.1", "localhost", "::1"):
        return False, (
            f"{description} '{url}' host '{hostname}' is not bound to a loopback address "
            "(127.0.0.1, localhost, ::1)."
        )

    # 4. Port validation
    try:
        port = parsed.port
    except ValueError as ve:
        return False, f"{description} '{url}' contains an invalid port: {ve}"

    if port is not None:
        if port < 1 or port > 65535:
            return False, f"{description} '{url}' port '{port}' is outside valid TCP range (1..65535)."
        if allowed_ports is not None and port not in allowed_ports:
            return False, (
                f"{description} '{url}' port '{port}' is not permitted "
                f"(allowed ports: {', '.join(str(p) for p in allowed_ports)})."
            )

    return True, None


class NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    """Rejects automatic HTTP redirects fail-closed to prevent SSRF and loopback escape."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise urllib.error.HTTPError(
            newurl,
            code,
            f"Automatic HTTP redirect ({code}) to '{newurl}' is blocked for security.",
            headers,
            fp
        )

    def http_error_301(self, req, fp, code, msg, headers):
        loc = headers.get("Location", "") if headers else ""
        return self.redirect_request(req, fp, 301, msg, headers, loc)

    def http_error_302(self, req, fp, code, msg, headers):
        loc = headers.get("Location", "") if headers else ""
        return self.redirect_request(req, fp, 302, msg, headers, loc)

    def http_error_303(self, req, fp, code, msg, headers):
        loc = headers.get("Location", "") if headers else ""
        return self.redirect_request(req, fp, 303, msg, headers, loc)

    def http_error_307(self, req, fp, code, msg, headers):
        loc = headers.get("Location", "") if headers else ""
        return self.redirect_request(req, fp, 307, msg, headers, loc)

    def http_error_308(self, req, fp, code, msg, headers):
        loc = headers.get("Location", "") if headers else ""
        return self.redirect_request(req, fp, 308, msg, headers, loc)


def safe_urlopen(req: urllib.request.Request, timeout: float = 30.0, **kwargs):
    """Executes an HTTP request strictly refusing redirects and preventing network escapes."""
    opener = urllib.request.build_opener(NoRedirectHandler)
    return opener.open(req, timeout=timeout, **kwargs)


def validate_assistant_request(
    request: AssistantRequest,
    capabilities: AssistantCapabilities
) -> Tuple[bool, Optional[AssistantResponse]]:
    """Central policy validation gate for all CIPHER assistant requests.
    
    Invariants enforced:
    1. Blank or whitespace-only prompts rejected.
    2. Timeouts bounded between 1.0s and 300.0s and strictly finite.
    3. Tool execution fail-closed: rejected if requested or if backend has tools active.
    4. Local-only policy: reject if require_local=True but route is not VERIFIED_LOCAL.
    """
    if not isinstance(request, AssistantRequest):
        return False, AssistantResponse(
            status=AssistantStatus.INVALID_REQUEST,
            error="Request must be an instance of AssistantRequest.",
            route=getattr(capabilities, "default_route", ProcessingRoute.UNKNOWN)
        )

    # Invariant 1: Blank prompts
    if not isinstance(request.prompt, str) or not request.prompt or not request.prompt.strip():
        return False, AssistantResponse(
            status=AssistantStatus.INVALID_REQUEST,
            error="Prompt cannot be empty or whitespace only.",
            route=capabilities.default_route
        )

    # Invariant 2: Timeout bounds & finiteness (reject bools, non-numbers, NaN, Inf)
    if (
        isinstance(request.timeout_seconds, bool)
        or not isinstance(request.timeout_seconds, (int, float))
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
