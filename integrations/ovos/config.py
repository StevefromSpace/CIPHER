"""CIPHER — OpenVoiceOS Integration Configuration.

Enforces loopback network binding, local intent execution, session propagation,
and explicit privacy acknowledgment regarding messagebus broadcast behavior.
"""

from dataclasses import dataclass, field
import os
from pathlib import Path
from typing import Optional, Tuple


@dataclass
class OVOSConfig:
    """Configuration settings for the optional OpenVoiceOS integration."""

    # Integration enablement (strictly disabled by default)
    enabled: bool = field(
        default_factory=lambda: os.environ.get("CIPHER_OVOS_ENABLED", "0").lower() in ("1", "true", "yes")
    )

    # OVOS Messagebus WebSocket URL (strictly loopback by default)
    bus_url: str = "ws://127.0.0.1:8181/core"

    # Default speech recognition/intent language
    language: str = "en-us"

    # Per-utterance timeout in seconds
    timeout_seconds: float = 15.0

    # Local-first enforcement
    strict_local: bool = True

    # Default identifier
    default_model: str = "local/ovos-intent"

    # Acknowledgment that the OVOS messagebus broadcasts frames to all connected clients
    broadcast_privacy_acknowledged: bool = True

    def validate_bus_url(self) -> Tuple[bool, Optional[str]]:
        """Ensures messagebus communication is strictly bound to loopback."""
        url = self.bus_url.lower()
        is_loopback = (
            "127.0.0.1" in url
            or "localhost" in url
            or "[::1]" in url
        )
        if not is_loopback:
            return False, f"OVOS Messagebus URL '{self.bus_url}' is not bound to a loopback address (127.0.0.1/localhost)."
        return True, None
