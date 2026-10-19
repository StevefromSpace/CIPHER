"""CIPHER — Assistant Integration Contract (OpenClaw bridge adapter).

Re-exports core contracts from integrations.contract for backward compatibility.
"""

from integrations.contract import (
    AssistantCapabilities,
    AssistantRequest,
    AssistantResponse,
    AssistantStatus,
    BaseAssistantBackend,
    ProcessingRoute,
    sanitize_sensitive_data,
    validate_assistant_request,
)

__all__ = [
    "AssistantCapabilities",
    "AssistantRequest",
    "AssistantResponse",
    "AssistantStatus",
    "BaseAssistantBackend",
    "ProcessingRoute",
    "sanitize_sensitive_data",
    "validate_assistant_request",
]
