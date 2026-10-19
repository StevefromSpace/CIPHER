"""CIPHER — Leon Assistant Bridge Adapter.

Implements the request/response bridge via Leon's Fastify HTTP API,
enforcing loopback binding, local-first isolation, version pinning (1.x stable contract),
and profile authentication handling.
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
from .config import LeonConfig


class LeonBridge(BaseAssistantBackend):
    """Optional bridge connecting CIPHER to a local Leon instance."""

    def __init__(self, config: Optional[LeonConfig] = None):
        self.config = config or LeonConfig()

    @property
    def capabilities(self) -> AssistantCapabilities:
        """Declares Leon bridge capabilities and constraints.

        Route provenance:
        External Fastify REST service over loopback. Loopback binding does not
        prove the runtime is not using external or hosted models.
        Route defaults to UNKNOWN unless verified_local is True.
        """
        route = ProcessingRoute.VERIFIED_LOCAL if self.config.verified_local else ProcessingRoute.UNKNOWN
        return AssistantCapabilities(
            name="Leon",
            version=self.config.pinned_version,
            supports_text=True,
            supports_streaming=False,
            supports_sessions=True,
            supports_voice_events=False,
            supports_tools=False,  # Skill actions locked down in CIPHER
            default_route=route,
            is_mock_only=False,
        )

    def is_available(self) -> bool:
        """Probes the Leon server health/info endpoint on loopback rejecting redirects."""
        if not self.config.enabled:
            return False

        is_loopback, _ = self.config.validate_api_url()
        if not is_loopback:
            return False

        try:
            info_url = urllib.parse.urljoin(self.config.api_url, self.config.info_endpoint)
            req = urllib.request.Request(info_url, headers={"User-Agent": "CIPHER-Bridge/0.4"})
            if self.config.api_key:
                req.add_header("X-API-Key", self.config.api_key)

            with safe_urlopen(req, timeout=2.0) as resp:
                if resp.status != 200:
                    return False
                # Validate version compatibility (1.x stable vs 2.x preview)
                try:
                    raw = resp.read().decode("utf-8")
                    if raw.strip():
                        data = json.loads(raw)
                        if isinstance(data, dict):
                            ver = data.get("version") or (data.get("data") or {}).get("version")
                            if ver and isinstance(ver, str):
                                if ver.strip().startswith("2.") and not self.config.pinned_version.startswith("2."):
                                    return False
                except Exception:
                    pass
                return True
        except Exception:
            return False

    def get_status(self) -> Dict[str, Any]:
        """Returns sanitized status and health information."""
        return {
            "name": "Leon",
            "enabled": self.config.enabled,
            "api_url": self.config.api_url,
            "has_auth_key": bool(self.config.api_key),
            "pinned_version": self.config.pinned_version,
            "default_model": self.config.default_model,
            "strict_local": self.config.strict_local,
            "timeout_seconds": self.config.timeout_seconds,
            "route": self.capabilities.default_route.value,
            "compatibility_note": "Targeted for Leon 1.x stable API. Leon 2.0 developer preview APIs are treated as unstable.",
        }

    def _sanitize_error(self, message: str) -> str:
        """Strips credentials, API keys, or tokens from error output."""
        return sanitize_sensitive_data(message)

    def submit_prompt(self, request: AssistantRequest) -> AssistantResponse:
        """Submits a single text prompt to Leon's query endpoint.

        Guarantees:
        - Central policy validation (blank prompt, timeout, tool lockdown, local route).
        - Rejects immediately if integration is disabled.
        - Enforces loopback address binding.
        - Respects profile authentication without credential exposure.
        - Parses responses into standard AssistantResponse.
        """
        start_time = time.perf_counter()

        # 1. Gate: Check if integration is enabled
        if not self.config.enabled:
            return AssistantResponse(
                status=AssistantStatus.DISABLED,
                error="Leon integration is disabled in CIPHER configuration (CIPHER_LEON_ENABLED=0).",
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

        # 4. Gate: Model selection and local-first verification
        model = request.model_override or self.config.default_model
        is_local, rejection_reason = self.config.is_model_local(model)
        if not is_local:
            return AssistantResponse(
                status=AssistantStatus.REJECTED_CLOUD_MODEL,
                model=model,
                route=ProcessingRoute.REMOTE_CLOUD,
                error=f"Local-First Policy Rejection: {rejection_reason}",
            )

        # 5. Build HTTP request payload for Leon Fastify query endpoint
        timeout = request.timeout_seconds if request.timeout_seconds is not None else self.config.timeout_seconds
        query_url = urllib.parse.urljoin(self.config.api_url, self.config.query_endpoint)

        payload = {
            "utterance": request.prompt,
        }
        if request.session_id:
            payload["session_id"] = request.session_id

        req_bytes = json.dumps(payload).encode("utf-8")
        http_req = urllib.request.Request(
            query_url,
            data=req_bytes,
            headers={
                "Content-Type": "application/json",
                "User-Agent": "CIPHER-Bridge/0.4",
            },
            method="POST"
        )

        if self.config.api_key:
            http_req.add_header("X-API-Key", self.config.api_key)

        # 6. Execute HTTP request with timeout and redirect rejection
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
                        error=f"Leon response exceeded maximum permitted byte limit ({max_output_bytes} bytes)."
                    )

                if not raw_body.strip():
                    return AssistantResponse(
                        status=AssistantStatus.ERROR,
                        model=model,
                        route=self.capabilities.default_route,
                        duration_ms=duration_ms,
                        error="Leon returned empty response body."
                    )

                data = json.loads(raw_body)
                reply_text = ""
                if isinstance(data, dict):
                    # Check for Leon 1.x response shapes
                    if "output" in data and isinstance(data["output"], dict):
                        output_obj = data["output"]
                        reply_text = output_obj.get("speech") or output_obj.get("text") or ""
                    elif "speech" in data and isinstance(data["speech"], str):
                        reply_text = data["speech"]
                    elif "text" in data and isinstance(data["text"], str):
                        reply_text = data["text"]
                    elif "reply" in data and isinstance(data["reply"], str):
                        reply_text = data["reply"]

                if not reply_text:
                    return AssistantResponse(
                        status=AssistantStatus.ERROR,
                        model=model,
                        route=self.capabilities.default_route,
                        duration_ms=duration_ms,
                        error="Malformed response from Leon: missing assistant reply text field.",
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
            err_msg = f"Leon HTTP error {he.code}: {raw_err[:300]}"
            if he.code in (301, 302, 303, 307, 308):
                err_msg = f"Leon HTTP redirect {he.code} blocked for security: {raw_err[:300]}"
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
                error=f"Leon connection failure: {err_str}",
            )
        except TimeoutError:
            duration_ms = (time.perf_counter() - start_time) * 1000.0
            return AssistantResponse(
                status=AssistantStatus.TIMEOUT,
                model=model,
                route=self.capabilities.default_route,
                duration_ms=duration_ms,
                error=f"Request to Leon timed out after {timeout:.1f} seconds."
            )
        except (json.JSONDecodeError, ValueError) as je:
            duration_ms = (time.perf_counter() - start_time) * 1000.0
            return AssistantResponse(
                status=AssistantStatus.ERROR,
                model=model,
                route=self.capabilities.default_route,
                duration_ms=duration_ms,
                error=f"Malformed response from Leon (invalid JSON): {str(je)}"
            )
        except Exception as e:
            duration_ms = (time.perf_counter() - start_time) * 1000.0
            return AssistantResponse(
                status=AssistantStatus.ERROR,
                model=model,
                route=self.capabilities.default_route,
                duration_ms=duration_ms,
                error=f"Unexpected error communicating with Leon: {self._sanitize_error(str(e))}"
            )
