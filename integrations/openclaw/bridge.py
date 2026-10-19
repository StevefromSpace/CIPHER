"""CIPHER — OpenClaw Assistant Bridge Adapter.

Implements the request/response bridge via the supported OpenClaw CLI interface,
enforcing local-first isolation, configurable timeouts, cloud rejection, tool execution lockdown,
and error containment.
"""

from datetime import datetime
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
from typing import Any, Dict, List, Optional

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
from .config import OpenClawConfig


class OpenClawBridge(BaseAssistantBackend):
    """Optional bridge connecting CIPHER to a local OpenClaw assistant."""

    def __init__(self, config: Optional[OpenClawConfig] = None):
        self.config = config or OpenClawConfig()
        self._resolved_cli_path: Optional[Path] = None

    @property
    def capabilities(self) -> AssistantCapabilities:
        """Declares OpenClaw bridge capabilities and provenance constraints.

        Route provenance:
        OpenClaw runs as an external Node.js agent. Model name prefixes like 'ollama/'
        do not constitute cryptographic proof of local inference. Route remains UNKNOWN
        unless config.verified_local is explicitly enabled after in-process verification.
        """
        route = ProcessingRoute.VERIFIED_LOCAL if self.config.verified_local else ProcessingRoute.UNKNOWN
        return AssistantCapabilities(
            name="OpenClaw",
            version="1.0.0",
            supports_text=True,
            supports_streaming=False,
            supports_sessions=True,
            supports_voice_events=False,
            supports_tools=False,  # Locked down in CIPHER
            default_route=route,
            is_mock_only=False,
        )

    def _resolve_cli(self) -> Optional[Path]:
        """Locates the openclaw executable path safely."""
        if self.config.cli_path and self.config.cli_path.exists():
            return self.config.cli_path

        found = shutil.which("openclaw")
        if found:
            return Path(found)

        # Common Windows npm global / local paths
        if os.name == "nt":
            appdata = os.environ.get("APPDATA")
            if appdata:
                npm_cmd = Path(appdata) / "npm" / "openclaw.cmd"
                if npm_cmd.exists():
                    return npm_cmd

        return None

    def is_available(self) -> bool:
        """Checks if OpenClaw CLI is installed and runnable."""
        if not self.config.enabled:
            return False

        cli = self._resolve_cli()
        if not cli:
            return False

        try:
            res = subprocess.run(
                [str(cli), "--version"],
                capture_output=True,
                text=True,
                timeout=5.0,
                shell=False
            )
            return res.returncode == 0
        except Exception:
            return False

    def get_status(self) -> Dict[str, Any]:
        """Returns sanitized status and health information."""
        cli = self._resolve_cli()
        return {
            "name": "OpenClaw",
            "enabled": self.config.enabled,
            "installed": cli is not None,
            "cli_path": str(cli) if cli else None,
            "gateway_url": self.config.gateway_url,
            "default_model": self.config.default_model,
            "strict_local": self.config.strict_local,
            "tool_profile": self.config.tool_profile,
            "timeout_seconds": self.config.timeout_seconds,
            "route": self.capabilities.default_route.value,
        }

    def _sanitize_error(self, message: str) -> str:
        """Strips potential credentials, tokens, or private paths from error output."""
        return sanitize_sensitive_data(message)

    def submit_prompt(self, request: AssistantRequest) -> AssistantResponse:
        """Submits a single text prompt to OpenClaw and returns the structured response.

        Guarantees:
        - Central policy validation (blank prompts, timeout bounds, tool lockdown, local route).
        - Rejects immediately if integration is disabled.
        - Verifies tool policy restricts tool execution (profile == 'minimal').
        - Rejects cloud models before executing any external process.
        - Enforces strict process timeouts.
        - Parses responses into a standardized, decoupled AssistantResponse.
        """
        start_time = time.perf_counter()

        # 1. Gate: Check if integration is enabled
        if not self.config.enabled:
            return AssistantResponse(
                status=AssistantStatus.DISABLED,
                error="OpenClaw integration is disabled in CIPHER configuration (CIPHER_OPENCLAW_ENABLED=0).",
                route=self.capabilities.default_route,
            )

        # 2. Central policy validation gate
        is_valid, validation_resp = validate_assistant_request(request, self.capabilities)
        if not is_valid and validation_resp:
            return validation_resp

        # 3. Gate: Loopback network check
        is_loopback, loopback_err = self.config.validate_gateway_url()
        if not is_loopback:
            return AssistantResponse(
                status=AssistantStatus.ERROR,
                error=f"Security violation: {loopback_err}",
                route=self.capabilities.default_route,
            )

        # 4. Gate: Tool policy verification
        is_tool_safe, tool_err = self.config.is_tool_policy_safe()
        if not is_tool_safe:
            return AssistantResponse(
                status=AssistantStatus.REJECTED_TOOLS_ENABLED,
                error=f"Tool Policy Rejection: {tool_err}",
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

        # 6. Gate: CLI executable presence
        cli = self._resolve_cli()
        if not cli:
            return AssistantResponse(
                status=AssistantStatus.UNAVAILABLE,
                model=model,
                route=self.capabilities.default_route,
                error="OpenClaw CLI executable not found on PATH or configured location.",
            )

        # 7. Build CLI command arguments (literal array, no shell execution)
        timeout = request.timeout_seconds if request.timeout_seconds is not None else self.config.timeout_seconds
        cmd: List[str] = [
            str(cli),
            "agent",
            "--message", request.prompt,
            "--model", model,
            "--timeout", str(int(timeout)),
            "--profile", self.config.tool_profile,
            "--json"
        ]

        if self.config.explicit_tool_denial and self.config.tool_denial_args:
            cmd.extend(list(self.config.tool_denial_args))

        if request.session_id:
            cmd.extend(["--session-id", request.session_id])

        # 8. Execute subprocess with strict timeout
        try:
            res = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=timeout,
                shell=False
            )
            duration_ms = (time.perf_counter() - start_time) * 1000.0

            # Enforce output size bounding (1MB max to prevent memory exhaustion)
            max_output_bytes = 1048576
            if len((res.stdout or "").encode("utf-8", errors="replace")) > max_output_bytes:
                return AssistantResponse(
                    status=AssistantStatus.ERROR,
                    model=model,
                    route=self.capabilities.default_route,
                    duration_ms=duration_ms,
                    error=f"OpenClaw output exceeded maximum permitted byte limit ({max_output_bytes} bytes)."
                )

            if res.returncode != 0:
                raw_err = self._sanitize_error(res.stderr or res.stdout)
                lower_err = raw_err.lower()

                # Classify connection vs authentication vs general error
                if "econnrefused" in lower_err or "connect" in lower_err or "offline" in lower_err or "unreachable" in lower_err:
                    status = AssistantStatus.CONNECTION_ERROR
                elif "auth" in lower_err or "unauthorized" in lower_err or "forbidden" in lower_err:
                    status = AssistantStatus.AUTH_ERROR
                else:
                    status = AssistantStatus.ERROR

                return AssistantResponse(
                    status=status,
                    model=model,
                    route=self.capabilities.default_route,
                    duration_ms=duration_ms,
                    error=f"OpenClaw execution failed (exit code {res.returncode}): {raw_err[:300]}",
                    raw_output=raw_err
                )

            # 9. Parse JSON output
            raw_out = res.stdout.strip()
            if not raw_out:
                return AssistantResponse(
                    status=AssistantStatus.ERROR,
                    model=model,
                    route=self.capabilities.default_route,
                    duration_ms=duration_ms,
                    error="OpenClaw returned empty output.",
                    raw_output=raw_out
                )

            try:
                # Find JSON block in output if wrapped in prose
                data = None
                json_match = re.search(r'(\{.*\})', raw_out, re.DOTALL)
                if json_match:
                    data = json.loads(json_match.group(1))
                else:
                    data = json.loads(raw_out)

                reply_text = ""
                if isinstance(data, dict):
                    if "reply" in data and isinstance(data["reply"], str):
                        reply_text = data["reply"]
                    elif "text" in data and isinstance(data["text"], str):
                        reply_text = data["text"]
                    elif "result" in data and isinstance(data["result"], dict) and "text" in data["result"]:
                        reply_text = data["result"]["text"]
                    elif "payloads" in data and isinstance(data["payloads"], list) and len(data["payloads"]) > 0:
                        first = data["payloads"][0]
                        if isinstance(first, dict) and "text" in first:
                            reply_text = first["text"]
                    elif "content" in data and isinstance(data["content"], str):
                        reply_text = data["content"]
                    elif "status" in data and data.get("status") in ("error", "failed"):
                        return AssistantResponse(
                            status=AssistantStatus.ERROR,
                            model=model,
                            route=self.capabilities.default_route,
                            duration_ms=duration_ms,
                            error=f"OpenClaw run reported failure: {data.get('error', 'unknown error')}",
                            raw_output=raw_out
                        )

                if not reply_text:
                    return AssistantResponse(
                        status=AssistantStatus.ERROR,
                        model=model,
                        route=self.capabilities.default_route,
                        duration_ms=duration_ms,
                        error="Malformed response from OpenClaw: missing assistant reply text field.",
                        raw_output=raw_out[:300]
                    )

                return AssistantResponse(
                    status=AssistantStatus.OK,
                    text=reply_text,
                    model=model,
                    route=self.capabilities.default_route,
                    duration_ms=duration_ms,
                    raw_output=raw_out
                )

            except (json.JSONDecodeError, ValueError) as je:
                return AssistantResponse(
                    status=AssistantStatus.ERROR,
                    model=model,
                    route=self.capabilities.default_route,
                    duration_ms=duration_ms,
                    error=f"Malformed response from OpenClaw (invalid JSON): {str(je)}",
                    raw_output=raw_out[:300]
                )

        except subprocess.TimeoutExpired:
            duration_ms = (time.perf_counter() - start_time) * 1000.0
            return AssistantResponse(
                status=AssistantStatus.TIMEOUT,
                model=model,
                route=self.capabilities.default_route,
                duration_ms=duration_ms,
                error=f"Request to OpenClaw timed out after {timeout:.1f} seconds."
            )
        except Exception as e:
            duration_ms = (time.perf_counter() - start_time) * 1000.0
            return AssistantResponse(
                status=AssistantStatus.ERROR,
                model=model,
                route=self.capabilities.default_route,
                duration_ms=duration_ms,
                error=f"Unexpected error executing OpenClaw subprocess: {self._sanitize_error(str(e))}"
            )
