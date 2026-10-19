"""CIPHER — Dodaios Assistant Bridge Adapter.

Implements the request/response bridge via Dodaios Control REST API,
enforcing loopback network binding, local-first isolation, least-privileged text interaction,
and strict rejection of computer use or mission dispatch.
"""

from datetime import datetime
import json
import os
import re
import time
from typing import Any, Dict, Optional
import urllib.error
import urllib.parse
import urllib.request

from integrations.contract import (
    AssistantCapabilities,
    AssistantRequest,
    AssistantResponse,
    AssistantStatus,
    BaseAssistantBackend,
    ProcessingRoute,
    NoRedirectHandler,
    safe_urlopen,
    sanitize_sensitive_data,
    validate_assistant_request,
)
from .config import DodaiosConfig


class DodaiosBridge(BaseAssistantBackend):
    """Optional bridge connecting CIPHER to a local Dodaios instance."""

    def __init__(self, config: Optional[DodaiosConfig] = None):
        self.config = config or DodaiosConfig()

    @property
    def capabilities(self) -> AssistantCapabilities:
        """Declares Dodaios bridge capabilities and security constraints.

        Route provenance:
        External REST service over loopback. Loopback binding does not prove
        the runtime is not forwarding upstream to proprietary cloud models.
        Route defaults to UNKNOWN unless verified_local is True.
        """
        route = ProcessingRoute.VERIFIED_LOCAL if self.config.verified_local else ProcessingRoute.UNKNOWN
        return AssistantCapabilities(
            name="Dodaios",
            version="0.1.0-preview",
            supports_text=True,
            supports_streaming=False,
            supports_sessions=True,
            supports_voice_events=False,
            supports_tools=False,  # Computer use & mission dispatch strictly disabled
            default_route=route,
            is_mock_only=False,
        )

    def is_available(self) -> bool:
        """Probes the Dodaios Control API health endpoint on loopback rejecting redirects."""
        if not self.config.enabled:
            return False

        is_loopback, _ = self.config.validate_api_url()
        if not is_loopback:
            return False

        try:
            health_url = urllib.parse.urljoin(self.config.api_url, self.config.health_endpoint)
            req = urllib.request.Request(health_url, headers={"User-Agent": "CIPHER-Bridge/0.4"})
            if self.config.api_key:
                req.add_header("Authorization", f"Bearer {self.config.api_key}")

            with safe_urlopen(req, timeout=2.0) as resp:
                return resp.status == 200
        except Exception:
            return False

    def get_status(self) -> Dict[str, Any]:
        """Returns sanitized status and health information."""
        return {
            "name": "Dodaios",
            "enabled": self.config.enabled,
            "api_url": self.config.api_url,
            "has_auth_key": bool(self.config.api_key),
            "default_model": self.config.default_model,
            "strict_local": self.config.strict_local,
            "timeout_seconds": self.config.timeout_seconds,
            "route": self.capabilities.default_route.value,
        }

    def _sanitize_error(self, message: str) -> str:
        """Strips credentials or secrets from error output."""
        return sanitize_sensitive_data(message)

    def submit_prompt(self, request: AssistantRequest) -> AssistantResponse:
        """Submits a single text prompt to Dodaios Control REST API.

        Guarantees:
        - Central policy validation (blank prompt, timeout, tool lockdown, local route).
        - Rejects immediately if integration is disabled.
        - Rejects privileged computer use or mission dispatch requests.
        - Restricts communication strictly to loopback addresses.
        - Communicates via least-privileged OpenAPI chat/query endpoint.
        - Sanitizes all errors and credentials.
        """
        start_time = time.perf_counter()

        # 1. Gate: Integration enabled check
        if not self.config.enabled:
            return AssistantResponse(
                status=AssistantStatus.DISABLED,
                error="Dodaios integration is disabled in CIPHER configuration (CIPHER_DODAIOS_ENABLED=0).",
                route=self.capabilities.default_route,
            )

        # 2. Central policy validation gate
        is_valid, validation_resp = validate_assistant_request(request, self.capabilities)
        if not is_valid and validation_resp:
            return validation_resp

        # 3. Gate: Loopback network check
        is_loopback, loopback_err = self.config.validate_api_url()
        if not is_loopback:
            return AssistantResponse(
                status=AssistantStatus.ERROR,
                error=f"Security violation: {loopback_err}",
                route=self.capabilities.default_route,
            )

        # 4. Gate: Rejection of computer use, mission dispatch, shell execution, config mutation, or installation
        blocked_actions = (
            "computer_use", "mission", "execute", "shell", "config", "install",
            "mission_dispatch", "shell_execution", "config_mutation", "installation"
        )
        if any(request.metadata.get(act) for act in blocked_actions):
            return AssistantResponse(
                status=AssistantStatus.REJECTED_TOOLS_ENABLED,
                error="Privileged actions (computer use, mission dispatch, shell execution, config mutation, installation) are strictly blocked in Dodaios adapter.",
                route=self.capabilities.default_route,
            )

        # 5. Gate: Model selection and local-first verification
        model = request.model_override or self.config.default_model
        is_local, rejection_reason = self.config.is_model_local(model)
        if not is_local:
            return AssistantResponse(
                status=AssistantStatus.REJECTED_CLOUD_MODEL,
                model=model,
                route=ProcessingRoute.REMOTE_CLOUD,
                error=f"Local-First Policy Rejection: {rejection_reason}",
            )

        # 6. Build HTTP request payload for least-privileged chat endpoint
        timeout = request.timeout_seconds if request.timeout_seconds is not None else self.config.timeout_seconds
        chat_url = urllib.parse.urljoin(self.config.api_url, self.config.chat_endpoint)

        payload = {
            "model": model,
            "messages": [
                {"role": "user", "content": request.prompt}
            ],
            "stream": False,
        }
        if request.session_id:
            payload["session_id"] = request.session_id

        req_bytes = json.dumps(payload).encode("utf-8")
        http_req = urllib.request.Request(
            chat_url,
            data=req_bytes,
            headers={
                "Content-Type": "application/json",
                "User-Agent": "CIPHER-Bridge/0.4",
            },
            method="POST"
        )

        if self.config.api_key:
            http_req.add_header("Authorization", f"Bearer {self.config.api_key}")

        # 7. Execute HTTP request with timeout and redirect rejection
        try:
            with safe_urlopen(http_req, timeout=timeout) as response:
                duration_ms = (time.perf_counter() - start_time) * 1000.0
                raw_body = response.read().decode("utf-8")

                # Enforce output size bounding (1MB)
                max_output_bytes = 1048576
                if len(raw_body.encode("utf-8", errors="replace")) > max_output_bytes:
                    return AssistantResponse(
                        status=AssistantStatus.ERROR,
                        model=model,
                        route=self.capabilities.default_route,
                        duration_ms=duration_ms,
                        error=f"Dodaios response exceeded maximum permitted byte limit ({max_output_bytes} bytes)."
                    )

                if not raw_body.strip():
                    return AssistantResponse(
                        status=AssistantStatus.ERROR,
                        model=model,
                        route=self.capabilities.default_route,
                        duration_ms=duration_ms,
                        error="Dodaios returned empty response body."
                    )

                data = json.loads(raw_body)
                # Parse OpenAI-compatible or Dodaios response shape
                reply_text = ""
                if isinstance(data, dict):
                    if "choices" in data and len(data["choices"]) > 0:
                        first_choice = data["choices"][0]
                        if "message" in first_choice and "content" in first_choice["message"]:
                            reply_text = first_choice["message"]["content"]
                        elif "text" in first_choice:
                            reply_text = first_choice["text"]
                    elif "reply" in data and isinstance(data["reply"], str):
                        reply_text = data["reply"]
                    elif "text" in data and isinstance(data["text"], str):
                        reply_text = data["text"]
                    elif "content" in data and isinstance(data["content"], str):
                        reply_text = data["content"]

                if not reply_text:
                    return AssistantResponse(
                        status=AssistantStatus.ERROR,
                        model=model,
                        route=self.capabilities.default_route,
                        duration_ms=duration_ms,
                        error="Malformed response from Dodaios: missing completion text.",
                        raw_output=raw_body[:300]
                    )

                return AssistantResponse(
                    status=AssistantStatus.OK,
                    text=reply_text,
                    model=model,
                    route=self.capabilities.default_route,
                    duration_ms=duration_ms,
                    raw_output=raw_body
                )

        except urllib.error.HTTPError as he:
            duration_ms = (time.perf_counter() - start_time) * 1000.0
            raw_err = ""
            if hasattr(he, "read"):
                try:
                    raw_err = he.read().decode("utf-8", errors="replace")
                except Exception:
                    pass
            if not raw_err.strip():
                raw_err = str(getattr(he, "msg", "") or getattr(he, "reason", "") or he)
            raw_err = self._sanitize_error(raw_err)
            if he.code in (401, 403):
                status = AssistantStatus.AUTH_ERROR
            else:
                status = AssistantStatus.ERROR
            err_msg = f"Dodaios HTTP error {he.code}: {raw_err[:300]}"
            if he.code in (301, 302, 303, 307, 308):
                err_msg = f"Dodaios HTTP redirect {he.code} blocked for security: {raw_err[:300]}"
            return AssistantResponse(
                status=status,
                model=model,
                route=self.capabilities.default_route,
                duration_ms=duration_ms,
                error=err_msg,
                raw_output=raw_err
            )
        except urllib.error.URLError as ue:
            duration_ms = (time.perf_counter() - start_time) * 1000.0
            err_str = self._sanitize_error(str(ue.reason))
            return AssistantResponse(
                status=AssistantStatus.CONNECTION_ERROR,
                model=model,
                route=self.capabilities.default_route,
                duration_ms=duration_ms,
                error=f"Dodaios connection failure: {err_str}",
            )
        except TimeoutError:
            duration_ms = (time.perf_counter() - start_time) * 1000.0
            return AssistantResponse(
                status=AssistantStatus.TIMEOUT,
                model=model,
                route=self.capabilities.default_route,
                duration_ms=duration_ms,
                error=f"Request to Dodaios timed out after {timeout:.1f} seconds."
            )
        except (json.JSONDecodeError, ValueError) as je:
            duration_ms = (time.perf_counter() - start_time) * 1000.0
            return AssistantResponse(
                status=AssistantStatus.ERROR,
                model=model,
                route=self.capabilities.default_route,
                duration_ms=duration_ms,
                error=f"Malformed response from Dodaios (invalid JSON): {str(je)}"
            )
        except Exception as e:
            duration_ms = (time.perf_counter() - start_time) * 1000.0
            return AssistantResponse(
                status=AssistantStatus.ERROR,
                model=model,
                route=self.capabilities.default_route,
                duration_ms=duration_ms,
                error=f"Unexpected error communicating with Dodaios: {self._sanitize_error(str(e))}"
            )
