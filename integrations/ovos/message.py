"""CIPHER — OpenVoiceOS (OVOS) Message Protocol.

Defines the standard OVOS messagebus envelope structure, session context,
and response correlation logic.
"""

from dataclasses import dataclass, field
import json
import time
from typing import Any, Dict, List, Optional
import uuid


@dataclass
class OVOSMessage:
    """Standard OVOS Messagebus JSON frame."""
    type: str
    data: Dict[str, Any] = field(default_factory=dict)
    context: Dict[str, Any] = field(default_factory=dict)

    def to_json(self) -> str:
        """Serializes the message envelope to JSON."""
        return json.dumps({
            "type": self.type,
            "data": self.data,
            "context": self.context,
        })

    @classmethod
    def from_json(cls, raw: str) -> "OVOSMessage":
        """Deserializes JSON into an OVOS message envelope."""
        parsed = json.loads(raw)
        return cls(
            type=parsed.get("type", ""),
            data=parsed.get("data", {}),
            context=parsed.get("context", {}),
        )

    @classmethod
    def create_utterance(
        cls,
        text: str,
        session_id: Optional[str] = None,
        lang: str = "en-us"
    ) -> "OVOSMessage":
        """Builds a recognizer_loop:utterance envelope for intent processing."""
        sid = session_id or f"cipher-session-{uuid.uuid4().hex[:12]}"
        return cls(
            type="recognizer_loop:utterance",
            data={
                "utterances": [text],
                "lang": lang,
            },
            context={
                "session_id": sid,
                "source": "cipher_bridge",
                "destination": ["skills"],
                "timestamp": time.time(),
            }
        )

    def is_correlated_response_to(self, request_msg: "OVOSMessage") -> bool:
        """Determines if this message is a correlated response to a sent request."""
        # Check matching session_id in context
        req_session = request_msg.context.get("session_id")
        resp_session = self.context.get("session_id")

        if req_session and resp_session and req_session == resp_session:
            return self.type in ("speak", "ovos.utterance.handled", "complete_intent_failure")

        # Fallback check on target or source
        return False
