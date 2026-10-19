"""CIPHER — OpenVoiceOS Integration Configuration.

Enforces loopback network binding, session propagation, and explicit safety boundaries
regarding live skill dispatch and messagebus broadcast privacy.

Notice on Safety Boundary & Destination Semantics:
The OVOS messagebus route 'recognizer_loop:utterance' with destination: ['skills'] routes
utterances to active OVOS skills that execute arbitrary actions on the host OS.
It is NOT a passive, no-tools text endpoint. Keyword-based denylists cannot make it safe.
Under CIPHER safety policy, live skill dispatch is disabled fail-closed (allow_live_skill_dispatch=False).
General assistant prompt submission is mock-only/unavailable until a verified non-action text route is established.

Notice on Confidentiality Guard:
Outbound sensitive-data checking is a best-effort pattern-based detection heuristic before
messagebus emission, NOT complete secret protection.
"""

from dataclasses import dataclass, field
import os
from pathlib import Path
from typing import Optional, Tuple

from integrations.contract import validate_loopback_url


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

    # Provenance verification: False until verified local runtime confirmed
    verified_local: bool = False

    # Default identifier
    default_model: str = "local/ovos-intent"

    # Live skill dispatch enablement: strictly False by default.
    # Utterance routing to skills triggers active skill actions; live execution is disabled.
    allow_live_skill_dispatch: bool = False

    # Acknowledgment that the OVOS messagebus broadcasts frames to all connected clients
    broadcast_privacy_acknowledged: bool = True

    def validate_bus_url(self) -> Tuple[bool, Optional[str]]:
        """Ensures messagebus communication is strictly bound to loopback using shared validator."""
        return validate_loopback_url(
            self.bus_url,
            allowed_schemes=("ws", "wss"),
            description="OVOS Messagebus URL"
        )
