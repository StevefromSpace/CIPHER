"""CIPHER — OpenClaw Optional Assistant Integration Package.

This package provides an isolated, optional bridge between CIPHER and OpenClaw.
"""

from .assistant_contract import AssistantRequest, AssistantResponse, AssistantStatus, BaseAssistantBackend
from .bridge import OpenClawBridge
from .config import OpenClawConfig

__all__ = [
    "AssistantRequest",
    "AssistantResponse",
    "AssistantStatus",
    "BaseAssistantBackend",
    "OpenClawBridge",
    "OpenClawConfig",
]
