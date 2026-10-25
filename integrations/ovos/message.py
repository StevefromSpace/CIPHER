"""CIPHER — OpenVoiceOS (OVOS) Message Protocol.

Defines the standard OVOS messagebus envelope structure, session context,
and explicit response correlation logic.

Notice on Destination Semantics:
The 'recognizer_loop:utterance' message sets destination: ['skills'], routing
utterances to active OVOS skills that can trigger actions on the host OS.
This route cannot be treated as a passive, tool-free text endpoint.
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
        """Builds a recognizer_loop:utterance envelope for intent processing.

        Notice: destination is explicitly ['skills'], routing to active OVOS skills.
        """
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
        """Determines if this message is an explicitly correlated response to a sent request.

        Requires:
        1. Both messages have valid context dictionaries.
        2. Both messages have non-empty, exactly matching session_id fields.
        3. Response type is an expected intent/speech response event.
        """
        if not isinstance(request_msg, OVOSMessage):
            return False

        req_session = request_msg.context.get("session_id") if isinstance(request_msg.context, dict) else None
        resp_session = self.context.get("session_id") if isinstance(self.context, dict) else None

        if not req_session or not resp_session or req_session != resp_session:
            return False

        return self.type in ("speak", "ovos.utterance.handled", "complete_intent_failure")
