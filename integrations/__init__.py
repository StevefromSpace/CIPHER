"""CIPHER — Assistant Integrations Layer.

Provides optional, provider-independent integration adapters for:
- OpenClaw (one-shot isolated CLI)
- Dodaios (Control REST API / OpenAPI)
- Leon (Fastify REST query API / Socket.IO)
- OpenVoiceOS (OVOS messagebus voice/event integration)

All integrations enforce local-first constraints, loopback network binding,
tool execution lockdown, and strict error containment without polluting CIPHER's C++ core.
"""

from .contract import (
    AssistantCapabilities,
    AssistantRequest,
    AssistantResponse,
    AssistantStatus,
    BaseAssistantBackend,
    ProcessingRoute,
    ResponseTooLargeError,
    read_bounded,
    sanitize_sensitive_data,
    validate_assistant_request,
)
from .dodaios import DodaiosBridge, DodaiosConfig
from .leon import LeonBridge, LeonConfig
from .openclaw import OpenClawBridge, OpenClawConfig
from .ovos import OVOSBridge, OVOSConfig, OVOSMessage

__all__ = [
    # Contracts
    "AssistantCapabilities",
    "AssistantRequest",
    "AssistantResponse",
    "AssistantStatus",
    "BaseAssistantBackend",
    "ProcessingRoute",
    "ResponseTooLargeError",
    "read_bounded",
    "sanitize_sensitive_data",
    "validate_assistant_request",
    # Adapters & Configs
    "OpenClawBridge",
    "OpenClawConfig",
    "DodaiosBridge",
    "DodaiosConfig",
    "LeonBridge",
    "LeonConfig",
    "OVOSBridge",
    "OVOSConfig",
    "OVOSMessage",
]
