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
import tempfile
import threading
import time
from typing import Any, Dict, List, Optional, Tuple

from integrations.contract import (
    AssistantCapabilities,
    AssistantRequest,
    AssistantResponse,
    AssistantStatus,
    BaseAssistantBackend,
    ProcessingRoute,
    ResponseTooLargeError,
    read_bounded,
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

    def _execute_bounded_process(
        self,
        cmd: List[str],
        env: Dict[str, str],
        timeout: float,
        max_bytes: int
    ) -> Tuple[int, bytes, bytes, Optional[str]]:
        """Executes a subprocess with bounded stream collection.

        Enforces output byte limits during collection by reading in chunks with read_bounded.
        If stdout or stderr exceeds max_bytes, the subprocess is killed immediately
        and input consumption ceases.
        """
        proc = subprocess.Popen(
            cmd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
            shell=False
        )

        stdout_chunks: List[bytes] = []
        stderr_chunks: List[bytes] = []
        stream_errors: List[Exception] = []

        def _reader(stream, storage):
            try:
                storage.append(read_bounded(stream, max_bytes=max_bytes))
            except Exception as ex:
                stream_errors.append(ex)

        t_out = threading.Thread(target=_reader, args=(proc.stdout, stdout_chunks))
        t_err = threading.Thread(target=_reader, args=(proc.stderr, stderr_chunks))
        t_out.daemon = True
        t_err.daemon = True
        t_out.start()
        t_err.start()

        try:
            timed_out = False
            try:
                proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                try:
                    proc.kill()
                except Exception:
                    pass
                proc.wait()

            t_out.join(timeout=1.0)
            t_err.join(timeout=1.0)

            if timed_out:
                return -1, b"", b"", f"OpenClaw execution timed out after {timeout:.1f}s."

            for err in stream_errors:
                if isinstance(err, ResponseTooLargeError):
                    try:
                        proc.kill()
                    except Exception:
                        pass
                    proc.wait()
                    return -1, b"", b"", f"OpenClaw output exceeded maximum permitted byte limit ({max_bytes} bytes)."

            if stream_errors:
                return -1, b"", b"", f"I/O error reading process streams: {stream_errors[0]}"

            out_b = stdout_chunks[0] if stdout_chunks else b""
            err_b = stderr_chunks[0] if stderr_chunks else b""
            return proc.returncode, out_b, err_b, None
        finally:
            try:
                if proc.stdout:
                    proc.stdout.close()
            except Exception:
                pass
            try:
                if proc.stderr:
                    proc.stderr.close()
            except Exception:
                pass

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

        if request.session_id:
            cmd.extend(["--session-id", request.session_id])

        # 8. Configure isolated environment and execute subprocess with bounded streaming
        env = os.environ.copy()
        temp_dir_obj = None
        try:
            if self.config.enforce_isolated_config:
                temp_dir_obj = tempfile.TemporaryDirectory(prefix="cipher_openclaw_")
                temp_dir = Path(temp_dir_obj.name)
                config_file = temp_dir / "openclaw.json"
                state_dir = temp_dir / "state"
                state_dir.mkdir(parents=True, exist_ok=True)

                isolated_conf = {
                    "gateway": {
                        "url": self.config.gateway_url
                    },
                    "tools": {
                        "profile": self.config.tool_profile,
                        "deny": list(self.config.tool_deny_patterns)
                    }
                }
                with open(config_file, "w", encoding="utf-8") as f:
                    json.dump(isolated_conf, f)

                env["OPENCLAW_CONFIG_PATH"] = str(config_file)
                env["OPENCLAW_STATE_DIR"] = str(state_dir)
                env["OPENCLAW_GATEWAY_URL"] = self.config.gateway_url
                env["OPENCLAW_PROFILE"] = self.config.tool_profile

            retcode, stdout_b, stderr_b, err_msg = self._execute_bounded_process(
                cmd=cmd,
                env=env,
                timeout=timeout,
                max_bytes=self.config.max_output_bytes,
            )
            duration_ms = (time.perf_counter() - start_time) * 1000.0

            if err_msg:
                if "timed out" in err_msg.lower():
                    status = AssistantStatus.TIMEOUT
                else:
                    status = AssistantStatus.ERROR
                return AssistantResponse(
                    status=status,
                    model=model,
                    route=self.capabilities.default_route,
                    duration_ms=duration_ms,
                    error=err_msg,
                )

            if retcode != 0:
                raw_err = self._sanitize_error(
                    stderr_b.decode("utf-8", errors="replace") or stdout_b.decode("utf-8", errors="replace")
                )
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
                    error=f"OpenClaw execution failed (exit code {retcode}): {raw_err[:300]}",
                    raw_output=raw_err
                )

            # 9. Parse JSON output
            raw_out = stdout_b.decode("utf-8", errors="replace").strip()
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

        except Exception as e:
            duration_ms = (time.perf_counter() - start_time) * 1000.0
            return AssistantResponse(
                status=AssistantStatus.ERROR,
                model=model,
                route=self.capabilities.default_route,
                duration_ms=duration_ms,
                error=f"Unexpected error executing OpenClaw subprocess: {self._sanitize_error(str(e))}"
            )
        finally:
            if temp_dir_obj is not None:
                try:
                    temp_dir_obj.cleanup()
                except Exception:
                    pass
