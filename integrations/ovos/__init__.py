"""CIPHER — OpenVoiceOS Integration.

Provides optional integration adapter for OpenVoiceOS messagebus.
"""

from .bridge import MockBusClient, OVOSBridge
from .config import OVOSConfig
from .message import OVOSMessage

__all__ = ["OVOSBridge", "OVOSConfig", "OVOSMessage", "MockBusClient"]
