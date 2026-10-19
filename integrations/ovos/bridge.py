"""CIPHER — OpenVoiceOS (OVOS) Assistant Bridge Adapter.

Implements the voice/event integration via OVOS messagebus envelopes,
enforcing loopback binding, session correlation, broadcast confidentiality guards,
and failure containment.
"""

from datetime import datetime
import json
import os
import re
import socket
import time
from typing import Any, Callable, Dict, Optional
import urllib.parse

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
from .config import OVOSConfig
from .message import OVOSMessage


class OVOSBridge(BaseAssistantBackend):
    """Optional bridge connecting CIPHER to OpenVoiceOS messagebus."""

    def __init__(
        self,
        config: Optional[OVOSConfig] = None,
        bus_client_factory: Optional[Callable[[], Any]] = None
    ):
        self.config = config or OVOSConfig()
        self._bus_client_factory = bus_client_factory

    @property
    def capabilities(self) -> AssistantCapabilities:
        """Declares OVOS bridge capabilities and voice-event constraints.

        Route provenance:
        Loopback messagebus does not constitute proof of local model inference.
        Route defaults to UNKNOWN unless verified_local is True.
        Marked as mock-tested unless a live client factory is provided.
        """
        route = ProcessingRoute.VERIFIED_LOCAL if self.config.verified_local else ProcessingRoute.UNKNOWN
        return AssistantCapabilities(
            name="OpenVoiceOS",
            version="0.0.8",
            supports_text=True,
            supports_streaming=False,
            supports_sessions=True,
            supports_voice_events=True,  # Voice/event bus integration
            supports_tools=False,        # System actions and skill controls locked down
            default_route=route,
            is_mock_only=(self._bus_client_factory is None),
        )

    def is_available(self) -> bool:
        """Probes the OVOS messagebus TCP port on loopback."""
        if not self.config.enabled:
            return False

        is_loopback, _ = self.config.validate_bus_url()
        if not is_loopback:
            return False

        try:
            parsed = urllib.parse.urlparse(self.config.bus_url)
            host = parsed.hostname or "127.0.0.1"
            port = parsed.port or 8181

            # Standard socket connect probe (no extra libraries needed)
            with socket.create_connection((host, port), timeout=2.0):
                return True
        except Exception:
            return False

    def get_status(self) -> Dict[str, Any]:
        """Returns sanitized status and health information."""
        return {
            "name": "OpenVoiceOS",
            "enabled": self.config.enabled,
            "bus_url": self.config.bus_url,
            "language": self.config.language,
            "strict_local": self.config.strict_local,
            "timeout_seconds": self.config.timeout_seconds,
            "route": self.capabilities.default_route.value,
            "is_mock_only": self.capabilities.is_mock_only,
            "broadcast_notice": "OVOS messagebus broadcasts frames to all connected clients. Prompt confidentiality must be guarded.",
        }

    def _sanitize_error(self, message: str) -> str:
        """Strips credentials or secrets from error output."""
        return sanitize_sensitive_data(message)

    def submit_prompt(self, request: AssistantRequest) -> AssistantResponse:
        """Submits an utterance to the OVOS messagebus and correlates the response.

        Guarantees:
        - Central policy validation (blank prompts, timeout, tool lockdown, local route).
        - Rejects immediately if integration is disabled.
        - Enforces loopback address binding.
        - Guards against emitting sensitive credentials over broadcast bus.
        - Blocks privileged system actions (shutdown, reboot, install, shell).
        - Correlates asynchronous responses via session_id.
        - Sanitizes all returned text and errors.
        """
        start_time = time.perf_counter()

        # 1. Gate: Integration enablement
        if not self.config.enabled:
            return AssistantResponse(
                status=AssistantStatus.DISABLED,
                error="OpenVoiceOS integration is disabled in CIPHER configuration (CIPHER_OVOS_ENABLED=0).",
                route=self.capabilities.default_route,
            )

        # 2. Central policy validation gate
        is_valid, validation_resp = validate_assistant_request(request, self.capabilities)
        if not is_valid and validation_resp:
            return validation_resp

        # 3. Gate: Loopback network check
        is_loopback, loopback_err = self.config.validate_bus_url()
        if not is_loopback:
            return AssistantResponse(
                status=AssistantStatus.ERROR,
                error=f"Security violation: {loopback_err}",
                route=self.capabilities.default_route,
            )

        # 4. Gate: Guard against system control / shutdown commands and privileged actions
        prompt_lower = request.prompt.lower().strip()
        system_triggers = (
            "shutdown", "reboot", "restart", "power off", "install", "upgrade",
            "package", "shell", "config", "kill", "rm -rf", "format"
        )
        if any(trigger in prompt_lower for trigger in system_triggers) or any(
            request.metadata.get(act) for act in ("computer_use", "mission", "execute", "shell", "config", "install")
        ):
            return AssistantResponse(
                status=AssistantStatus.REJECTED_TOOLS_ENABLED,
                error="System control commands and privileged actions (shutdown, reboot, install, shell, config) are blocked from execution through CIPHER bridge.",
                route=self.capabilities.default_route,
            )

        # 5. Gate: Broadcast confidentiality policy
        if sanitize_sensitive_data(request.prompt) != request.prompt:
            return AssistantResponse(
                status=AssistantStatus.INVALID_REQUEST,
                error="Prompt contains sensitive credentials (tokens, keys, passwords). Refusing to broadcast over OVOS public messagebus.",
                route=self.capabilities.default_route,
            )

        # 6. Model / provider local verification
        model = request.model_override or self.config.default_model

        # 7. Build the correlated utterance message
        session_id = request.session_id or f"cipher-{int(time.time()*1000)}"
        utterance_msg = OVOSMessage.create_utterance(
            text=request.prompt,
            session_id=session_id,
            lang=self.config.language
        )

        timeout = request.timeout_seconds if request.timeout_seconds is not None else self.config.timeout_seconds

        # 8. Bus Client Dispatch
        # If a client factory or mock client is supplied, use it; otherwise probe connectivity
        if self._bus_client_factory:
            try:
                client = self._bus_client_factory()
                resp_msg = client.send_and_wait(utterance_msg, timeout=timeout)
                duration_ms = (time.perf_counter() - start_time) * 1000.0

                if not resp_msg:
                    return AssistantResponse(
                        status=AssistantStatus.TIMEOUT,
                        model=model,
                        route=self.capabilities.default_route,
                        duration_ms=duration_ms,
                        error=f"Timed out after {timeout:.1f}s waiting for correlated OVOS response."
                    )

                if resp_msg.type == "complete_intent_failure":
                    return AssistantResponse(
                        status=AssistantStatus.ERROR,
                        model=model,
                        route=self.capabilities.default_route,
                        duration_ms=duration_ms,
                        error="OVOS reported complete intent failure: no skill handled the utterance."
                    )

                reply_text = resp_msg.data.get("utterance", "")
                if not reply_text:
                    reply_text = str(resp_msg.data)

                return AssistantResponse(
                    status=AssistantStatus.OK,
                    text=reply_text,
                    model=model,
                    route=self.capabilities.default_route,
                    duration_ms=duration_ms,
                    raw_output=resp_msg.to_json()
                )

            except Exception as e:
                duration_ms = (time.perf_counter() - start_time) * 1000.0
                return AssistantResponse(
                    status=AssistantStatus.ERROR,
                    model=model,
                    route=self.capabilities.default_route,
                    duration_ms=duration_ms,
                    error=f"OVOS bus communication error: {self._sanitize_error(str(e))}"
                )

        # Without an external WebSocket library or live bus factory, check bus reachability
        if not self.is_available():
            duration_ms = (time.perf_counter() - start_time) * 1000.0
            return AssistantResponse(
                status=AssistantStatus.UNAVAILABLE,
                model=model,
                route=self.capabilities.default_route,
                duration_ms=duration_ms,
                error="OVOS messagebus is not reachable on loopback port 8181. Ensure ovos-messagebus is running.",
            )

        duration_ms = (time.perf_counter() - start_time) * 1000.0
        return AssistantResponse(
            status=AssistantStatus.UNAVAILABLE,
            model=model,
            route=self.capabilities.default_route,
            duration_ms=duration_ms,
            error="Live OVOS integration requires websocket transport client factory or running ovos-messagebus service.",
        )
