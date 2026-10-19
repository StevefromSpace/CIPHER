"""CIPHER — OpenVoiceOS (OVOS) Assistant Bridge Adapter.

Implements the voice/event integration via OVOS messagebus envelopes,
enforcing loopback binding, explicit session correlation, broadcast confidentiality guards,
and strict safety boundaries preventing live prompt dispatch to active skills.

Safety Boundary:
The OVOS messagebus route 'recognizer_loop:utterance' with destination: ['skills'] routes
utterances to active OVOS skills that execute arbitrary code/actions on the host OS.
It is NOT a passive, no-tools text endpoint. Keyword-based denylists cannot make this safe.
Under CIPHER safety policy, live skill dispatch is disabled fail-closed. General assistant
prompt submission remains mock-only until a verified non-action text route is established.
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
        General assistant submission is mock-only; live skill dispatch is prohibited.
        """
        route = ProcessingRoute.VERIFIED_LOCAL if self.config.verified_local else ProcessingRoute.UNKNOWN
        return AssistantCapabilities(
            name="OpenVoiceOS",
            version="0.0.8",
            supports_text=True,
            supports_streaming=False,
            supports_sessions=True,
            supports_voice_events=True,  # Expose voice/event bus capability
            supports_tools=False,        # System actions and skill controls locked down
            default_route=route,
            is_mock_only=True,           # Strictly mock-only for assistant text prompts
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
            "is_mock_only": True,
            "safety_boundary": "Live utterance submission to skills is disabled under safety policy; assistant prompts are mock-only.",
            "confidentiality_notice": "Outbound sensitive-data check is best-effort pattern detection, not complete secret protection. Bus frames are broadcast to all local clients.",
        }

    def _sanitize_error(self, message: str) -> str:
        """Strips credentials or secrets from error output."""
        return sanitize_sensitive_data(message)

    def submit_prompt(self, request: AssistantRequest) -> AssistantResponse:
        """Submits an utterance to the OVOS messagebus and correlates the response.

        Guarantees:
        - Central policy validation (blank prompts, timeout, tool lockdown, local route).
        - Rejects immediately if integration is disabled.
        - Fails closed on any tool execution request.
        - Enforces loopback address binding.
        - Guards against emitting sensitive credentials over broadcast bus (best-effort detection).
        - Enforces explicit response/session correlation.
        - Keeps live skill submission disabled/unavailable fail-closed under CIPHER safety policy.
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
        is_loopback, loopback_err = self.config.validate_api_url() if hasattr(self.config, "validate_api_url") else self.config.validate_bus_url()
        if not is_loopback:
            return AssistantResponse(
                status=AssistantStatus.ERROR,
                error=f"Security violation: {loopback_err}",
                route=self.capabilities.default_route,
            )

        # 4. Gate: Rejection of tool / privileged action requests
        blocked_actions = ("computer_use", "mission", "execute", "shell", "config", "install", "tools", "functions")
        if request.allow_tools or any(request.metadata.get(act) for act in blocked_actions):
            return AssistantResponse(
                status=AssistantStatus.REJECTED_TOOLS_ENABLED,
                error="Tool execution and privileged system actions are strictly prohibited in OpenVoiceOS adapter.",
                route=self.capabilities.default_route,
            )

        # 5. Gate: Best-effort broadcast confidentiality policy
        if sanitize_sensitive_data(request.prompt) != request.prompt:
            return AssistantResponse(
                status=AssistantStatus.INVALID_REQUEST,
                error="Prompt contains patterns resembling sensitive credentials (best-effort detection). Refusing to broadcast over OVOS public messagebus.",
                route=self.capabilities.default_route,
            )

        # 6. Safety Boundary Gate: General assistant prompt submission to live skills is prohibited.
        # recognizer_loop:utterance destination: ['skills'] triggers active skill actions.
        # Only verified mock client interaction is permitted; real/live client factories must NOT bypass this policy.
        if not self._bus_client_factory:
            duration_ms = (time.perf_counter() - start_time) * 1000.0
            return AssistantResponse(
                status=AssistantStatus.UNAVAILABLE,
                model=request.model_override or self.config.default_model,
                route=self.capabilities.default_route,
                duration_ms=duration_ms,
                error=(
                    "OVOS live skill execution is disabled under CIPHER safety policy: "
                    "destination: ['skills'] routes utterances to active skill actions with unverified tool isolation. "
                    "Live execution is disabled; general assistant prompt submission is mock-only."
                ),
            )

        client = self._bus_client_factory()

        # Enforce that supplied client is an authentic test mock, NOT a live or live-like client
        is_mock_client = bool(getattr(client, "is_mock", False) or getattr(client, "_is_cipher_mock", False))
        if not is_mock_client or not self.config.allow_mock or (self.config.allow_live_skill_dispatch and not self.config.verified_non_action_route):
            duration_ms = (time.perf_counter() - start_time) * 1000.0
            return AssistantResponse(
                status=AssistantStatus.UNAVAILABLE,
                model=request.model_override or self.config.default_model,
                route=self.capabilities.default_route,
                duration_ms=duration_ms,
                error=(
                    "Live OVOS bus dispatch rejected: supplying a client factory cannot bypass "
                    "the mock-only policy. Live skill execution is disabled under CIPHER safety policy."
                ),
            )

        # 7. Model / provider reference
        model = request.model_override or self.config.default_model

        # 8. Build the correlated utterance message
        session_id = request.session_id or f"cipher-{int(time.time()*1000)}"
        utterance_msg = OVOSMessage.create_utterance(
            text=request.prompt,
            session_id=session_id,
            lang=self.config.language
        )

        timeout = request.timeout_seconds if request.timeout_seconds is not None else self.config.timeout_seconds

        # 9. Mock Bus Client Dispatch with Explicit Response Correlation
        try:
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

            # Explicit session/response correlation verification
            if not resp_msg.is_correlated_response_to(utterance_msg):
                return AssistantResponse(
                    status=AssistantStatus.ERROR,
                    model=model,
                    route=self.capabilities.default_route,
                    duration_ms=duration_ms,
                    error="Uncorrelated OVOS response: session_id mismatch or invalid response event type.",
                    raw_output=resp_msg.to_json() if hasattr(resp_msg, "to_json") else str(resp_msg)
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
